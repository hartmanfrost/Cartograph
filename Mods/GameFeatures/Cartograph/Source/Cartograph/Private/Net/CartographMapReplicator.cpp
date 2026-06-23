#include "Net/CartographMapReplicator.h"

#include "CartographConfig.h"
#include "Core/CartographTypes.h"
#include "Core/CartographBuildingStore.h"
#include "Core/CartographSpatialGrid.h"
#include "Core/CartographTileManager.h"

#include "GameFramework/PlayerController.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "NativeGameplayTags.h"
#include "Engine/World.h"

// SPIKE(Q1): ReliableMessaging has NO mod-wiring path in the repo. This include
// + the GetFromPlayer call below assume the game auto-attaches a
// UReliableMessagingPlayerComponent to every PlayerController and that the
// component's transport reports Connected without the mod driving the handshake.
// This must be verified on a live dedicated-server connection before any other
// net work; it gates all of Phase 0 (SPEC 4.4, Q1). If the component is not
// auto-attached, the GameFeature must AddComponent + drive the protected
// Server_AdvertiseNewConnection handshake (out of scope for this file - the
// resolution path here is a non-owning lookup only).
#include "ReliableMessagingPlayerComponent.h"

// =============================================================================
// CartographMapReplicator.cpp - server-authoritative per-tile delta transport
// over the in-repo ReliableMessaging plugin (SPEC 4.4 / 4.5 / 5).
//
// DELETES the old slice-RCO bug class (#9) BY CONSTRUCTION:
//   - all sizes/counts are int32/uint32/uint64 -> NO int16 slice overflow at
//     >63 MB (legacy int16-sliceindex-overflow);
//   - no per-PC raw TMap<APlayerController*, ...> with a 10 s timer -> the
//     transport component owns its buffers with disconnect teardown, so the
//     timeout-race null-deref (legacy rco-null-deref-timeout-race) and per-join
//     leak cannot occur;
//   - NO synchronous Archive << CurrentBuildingData per join: the server holds
//     ONE shared immutable TSharedRef<const TArray<uint8>> snapshot per tile,
//     pinned across all joiners (join memory O(joiners + AoI));
//   - NO reliable NetMulticast per build event: live deltas are coalesced
//     per-tile version bumps pushed only to AoI clients.
//
// CONTRACT NOTE (reported in contractDeviations): the frozen header declares the
// PUBLIC surface but no private members/helpers for either class, and a UCLASS
// cannot gain members from a .cpp. So per-instance state for BOTH classes lives
// in file-scope side-tables keyed by the object pointer, created lazily and torn
// down on EndPlay / replicator destruction. The public signatures stay
// byte-identical; only the storage location differs from the header's
// "Implementation (owning agent)" comment.
// =============================================================================


DECLARE_LOG_CATEGORY_EXTERN(LogCartographNet, Display, All);
DEFINE_LOG_CATEGORY(LogCartographNet);

#define CARTO_NET_LOG(format, ...) UE_LOG(LogCartographNet, Display, TEXT("(%u)") TEXT(format), __LINE__ __VA_OPT__(, __VA_ARGS__))
#define CARTO_NET_LOG_WARNING(format, ...) UE_LOG(LogCartographNet, Warning, TEXT("(%u)") TEXT(format), __LINE__ __VA_OPT__(, __VA_ARGS__))
#define CARTO_NET_LOG_ERROR(format, ...) UE_LOG(LogCartographNet, Error, TEXT("(%u)") TEXT(format), __LINE__ __VA_OPT__(, __VA_ARGS__))


// -----------------------------------------------------------------------------
// Protocol constants.
// -----------------------------------------------------------------------------
namespace CartographNet
{
	/** Wire protocol version. Bumped on any layout change. A 1-byte prefix on
	 *  EVERY message; a mismatched byte means the payload is rejected (never
	 *  blindly applied) so a stale/incompatible client can't corrupt its store. */
	static constexpr uint8 ProtocolVersion = 1;

	/**
	 * App-level in-flight ring depth: the maximum number of tile blobs the server
	 * keeps un-acked in the ReliableMessaging FIFO at once per client, so the
	 * by-value send + PayloadsPendingConnection + chunker WriteBuffer
	 * amplification stays bounded regardless of total AoI (SPEC 4.4 caveat).
	 *
	 * SPIKE(Q9): the value 2 is a starting point for the 1-2-message in-flight
	 * window. ReliableMessaging exposes no per-message ack callback to the mod
	 * (acks are internal to the transport), so this implementation cannot refill
	 * strictly on ack; it instead drains the ring a few entries per net-tick
	 * (PushPendingDeltas) which keeps the materialized-bytes ceiling bounded but
	 * does NOT give true ack-driven flow control. The correct N and the
	 * refill-on-ack hook must be validated by load-testing FIFO head-of-line
	 * blocking under simultaneous join + belt-drag (SPEC 4.4 / Q9). If the
	 * transport later exposes an ack delegate, switch RingDrainPerTick refill to
	 * be ack-driven.
	 */
	static constexpr int32 InFlightRingDepth = 2;

	/** How many ring entries to release into the transport per net-tick. Keeps the
	 *  in-flight window shallow (head-of-line mitigation) while still draining.
	 *  Superseded at runtime by r.Cartograph.Net.RingDrainPerTick (this is the
	 *  documented default it mirrors); [[maybe_unused]] now that the read is the CVar. */
	[[maybe_unused]] static constexpr int32 RingDrainPerTick = InFlightRingDepth;

	/** Hard cap on records packed into a single tile blob. A tile is one render
	 *  cell; in practice occupancy is small, but this bounds a single by-value
	 *  send + chunker buffer so a pathological mega-tile cannot balloon RAM. */
	static constexpr int32 MaxRecordsPerTileBlob = 1 << 16;  // 65536; int32-safe
}


// -----------------------------------------------------------------------------
// Mod-owned native gameplay tags (SPEC 4.4: a SMALL fixed tag set, NOT a
// tag-per-tile). Defined as native tags so they are registered on BOTH ends at
// module load - the ReliableMessaging protocol reader resolves an incoming tag
// via FGameplayTag::RequestGameplayTag(name, false) and REJECTS unknown tags
// (ReliableMessagingProtocol.h ParseTagRegistration), so a runtime-only
// AddNativeGameplayTag on one side would be dropped by the peer. Native tags in
// the Cartograph.Net.* namespace cannot collide with CSS's FGReliableMessagingTags
// (which live under a different, game-owned namespace).
// -----------------------------------------------------------------------------
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_Cartograph_Net_Manifest, "Cartograph.Net.Manifest");
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_Cartograph_Net_Tile, "Cartograph.Net.Tile");
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_Cartograph_Net_Version, "Cartograph.Net.Version");
// AoI request is a fourth fixed tag (client -> server). It is still part of the
// "small fixed set"; one round-trip registration cost, not per-tile.
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_Cartograph_Net_AoI, "Cartograph.Net.AoI");

namespace CartographNetTags
{
	FGameplayTag Manifest() { return TAG_Cartograph_Net_Manifest; }
	FGameplayTag Tile()     { return TAG_Cartograph_Net_Tile; }
	FGameplayTag Version()  { return TAG_Cartograph_Net_Version; }
}

namespace
{
	/** Internal: the AoI-request tag (not in the public manifest/tile/version
	 *  surface but part of the fixed mod-owned set). */
	FGameplayTag AoITag() { return TAG_Cartograph_Net_AoI; }
}


// =============================================================================
// Wire format helpers. ALL sizes/counts are uint32/uint64 - the int16 slice
// overflow bug class is gone by construction. Every message is prefixed with
// the 1-byte protocol version.
//
// Tile blob layout (server -> client):
//   u8   ProtocolVersion
//   u32  TileId
//   u64  TileVersion
//   u32  RecordCount
//   RecordCount x {
//     f32 Pos.X, f32 Pos.Y, f32 Pos.Z   (single-precision; slim record native)
//     u16 PackedYaw
//     u16 ClassId
//     u8  Type (EBuildingDrawType)
//     u32 ExtraIndex   (server-local; remapped to a local index on apply)
//     -- typed extra (by Type) --
//     Spline: i32 NumPoints, NumPoints x {f32 X, f32 Y}
//     Wire:   f32 End.X, f32 End.Y
//     Beam:   f32 Length
//     Icon/Rectangle/Invalid: nothing
//   }
//
// Manifest / Version layout (both use the same pair encoding):
//   u8   ProtocolVersion
//   u32  PairCount
//   PairCount x { u32 TileId, u64 TileVersion }
//
// AoI request (client -> server):
//   u8   ProtocolVersion
//   f32  Min.X, f32 Min.Y, f32 Max.X, f32 Max.Y   (SCREEN-space viewport box)
//
// NOTE: this is a deliberately simple, robust framing. ReliableMessaging already
// chunks/acks/windows the byte stream; the mod does NOT re-implement reliability.
// Per-record bit-packing/quantization (legacy NetSerialize) can be layered in
// later as a pure size optimization without changing the protocol shape; it is
// intentionally omitted here so the Phase-0 transport is auditable and the
// reader can never run off the end of a malformed payload.
// =============================================================================
namespace
{
	/** A self-describing wire view of one building, decoded from / encoded to a
	 *  tile blob. The replicator's local slim store apply path consumes these. */
	struct FWireRecord
	{
		FBuildingRecord Record;
		// Typed extra (only the one matching Record.Type is meaningful).
		FSplineExtra Spline;
		FWireExtra Wire;
		FBeamExtra Beam;
	};

	void WriteRecord(FMemoryWriter& Ar, const FWireRecord& In)
	{
		FBuildingRecord Record = In.Record;
		Ar << Record.Pos;          // FVector3f - 3 x f32

		uint16 PackedYaw = Record.PackedYaw;
		uint16 ClassId = Record.ClassId;
		uint8 Type = (uint8)Record.Type;
		uint32 ExtraIndex = Record.ExtraIndex;
		Ar << PackedYaw;
		Ar << ClassId;
		Ar << Type;
		Ar << ExtraIndex;

		switch (Record.Type)
		{
		case EBuildingDrawType::Spline:
		{
			int32 NumPoints = In.Spline.Points.Num();
			Ar << NumPoints;
			for (int32 PointIndex = 0; PointIndex < NumPoints; ++PointIndex)
			{
				FVector2f Point = In.Spline.Points[PointIndex];
				Ar << Point;
			}
			break;
		}
		case EBuildingDrawType::Wire:
		{
			FVector2f End = In.Wire.End;
			Ar << End;
			break;
		}
		case EBuildingDrawType::Beam:
		{
			float Length = In.Beam.Length;
			Ar << Length;
			break;
		}
		default:
			break;
		}
	}

	/** Decode one record. Bounds-aware: every read is guarded against the archive
	 *  end so a truncated/hostile payload sets the error flag instead of reading
	 *  out of bounds. Returns false on any malformed field. */
	bool ReadRecord(FMemoryReader& Ar, FWireRecord& Out)
	{
		Ar << Out.Record.Pos;
		Ar << Out.Record.PackedYaw;
		Ar << Out.Record.ClassId;
		uint8 Type = 0;
		Ar << Type;
		Ar << Out.Record.ExtraIndex;
		if (Ar.IsError())
		{
			return false;
		}

		if (Type > (uint8)EBuildingDrawType::Beam)
		{
			// Unknown draw type on the wire - reject rather than guess.
			return false;
		}
		Out.Record.Type = (EBuildingDrawType)Type;

		switch (Out.Record.Type)
		{
		case EBuildingDrawType::Spline:
		{
			int32 NumPoints = 0;
			Ar << NumPoints;
			if (Ar.IsError() || NumPoints < 0 || NumPoints > (SPLINE_SEGMENTS + 1) * 4)
			{
				// Cap spline point count generously; reject absurd values so a
				// bad count cannot drive an unbounded allocation.
				return false;
			}
			Out.Spline.Points.SetNumUninitialized(NumPoints);
			for (int32 PointIndex = 0; PointIndex < NumPoints; ++PointIndex)
			{
				Ar << Out.Spline.Points[PointIndex];
			}
			break;
		}
		case EBuildingDrawType::Wire:
		{
			Ar << Out.Wire.End;
			break;
		}
		case EBuildingDrawType::Beam:
		{
			Ar << Out.Beam.Length;
			break;
		}
		default:
			break;
		}

		return !Ar.IsError();
	}

	/** Append a fixed (tileId, version) pair table (manifest / version-ping). */
	void WritePairTable(FMemoryWriter& Ar, const TArray<FTileId>& Tiles, const TArray<FTileVersion>& Versions)
	{
		uint8 Proto = CartographNet::ProtocolVersion;
		Ar << Proto;

		const int32 Count = FMath::Min(Tiles.Num(), Versions.Num());
		uint32 PairCount = (uint32)Count;
		Ar << PairCount;
		for (int32 PairIndex = 0; PairIndex < Count; ++PairIndex)
		{
			uint32 Tile = (uint32)Tiles[PairIndex];
			uint64 Version = (uint64)Versions[PairIndex];
			Ar << Tile;
			Ar << Version;
		}
	}

	/** Decode a pair table. Validates the protocol prefix and bounds. */
	bool ReadPairTable(FMemoryReader& Ar, TArray<FTileId>& OutTiles, TArray<FTileVersion>& OutVersions)
	{
		uint8 Proto = 0;
		Ar << Proto;
		if (Ar.IsError() || Proto != CartographNet::ProtocolVersion)
		{
			CARTO_NET_LOG_WARNING("Rejected pair table: protocol mismatch (got %u, want %u)", Proto, CartographNet::ProtocolVersion);
			return false;
		}

		uint32 PairCount = 0;
		Ar << PairCount;
		if (Ar.IsError() || (int64)PairCount > (int64)TILE_COUNT)
		{
			// A pair table can never legitimately exceed TILE_COUNT entries.
			return false;
		}

		OutTiles.Reset((int32)PairCount);
		OutVersions.Reset((int32)PairCount);
		for (uint32 PairIndex = 0; PairIndex < PairCount; ++PairIndex)
		{
			uint32 Tile = 0;
			uint64 Version = 0;
			Ar << Tile;
			Ar << Version;
			if (Ar.IsError() || Tile >= (uint32)TILE_COUNT)
			{
				return false;
			}
			OutTiles.Add((FTileId)Tile);
			OutVersions.Add((FTileVersion)Version);
		}
		return true;
	}

	/** Convert a tile's atlas pixel rect to its WORLD-space (cm, XY) box so the
	 *  spatial grid (world-keyed) can be queried for the tile's occupancy.
	 *  ScreenToWorld is axis-monotonic, so min/max map cleanly. */
	FBox2f TileWorldBox(FTileId Tile)
	{
		const FBox2f ScreenRect = CartographCoords::ScreenRectFromTile(Tile);
		const FVector2D WorldMin = CartographCoords::ScreenToWorld(ScreenRect.Min);
		const FVector2D WorldMax = CartographCoords::ScreenToWorld(ScreenRect.Max);
		const FVector2f Lo((float)FMath::Min(WorldMin.X, WorldMax.X), (float)FMath::Min(WorldMin.Y, WorldMax.Y));
		const FVector2f Hi((float)FMath::Max(WorldMin.X, WorldMax.X), (float)FMath::Max(WorldMin.Y, WorldMax.Y));
		return FBox2f(Lo, Hi);
	}

	/** Center (in tile-grid coordinates) of a screen-space box, for distance
	 *  ordering of the AoI manifest. */
	FVector2f ScreenBoxCenterTile(const FBox2f& ScreenBox)
	{
		const FVector2f Center = ScreenBox.GetCenter();
		return FVector2f(Center.X / (float)TILE_SIZE_PX, Center.Y / (float)TILE_SIZE_PX);
	}
}


// =============================================================================
// Side-stored per-instance state (see CONTRACT NOTE above).
// =============================================================================
namespace
{
	// ---- Server-side replicator state ----
	struct FReplicatorState
	{
		FCartographBuildingStore* Store = nullptr;
		FCartographSpatialGrid* Grid = nullptr;
		FCartographTileManager* TileManager = nullptr;

		// Lazy per-tile shared immutable snapshots, sized TILE_COUNT. One shared
		// ref pinned across ALL joiners (join memory O(joiners + AoI)).
		TArray<TSharedPtr<const FCartographMapReplicator::FTileSnapshot>> Snapshots;
	};

	TMap<const FCartographMapReplicator*, TSharedPtr<FReplicatorState>>& ReplicatorStates()
	{
		static TMap<const FCartographMapReplicator*, TSharedPtr<FReplicatorState>> Map;
		return Map;
	}

	FReplicatorState& GetReplicatorState(const FCartographMapReplicator* Self)
	{
		TSharedPtr<FReplicatorState>& State = ReplicatorStates().FindOrAdd(Self);
		if (!State.IsValid())
		{
			State = MakeShared<FReplicatorState>();
		}
		return *State;
	}

	// ---- Per-component (transport driver) state ----
	struct FLocalEntry
	{
		FBuildingHandle Handle;
		FBox2f Box = FBox2f(ForceInit);
	};

	struct FComponentState
	{
		// Non-owning transport (resolved via GetFromPlayer, SPIKE Q1).
		TWeakObjectPtr<UReliableMessagingPlayerComponent> Transport;

		// ---- Client side ----
		FBox2f LocalAoI = FBox2f(ForceInit);
		TArray<FTileVersion> ClientCachedVersions;   // sized TILE_COUNT; 0 == unseen
		TBitArray<> ClientPendingRepull;             // sized TILE_COUNT

		// Local slim store + grid + tile manager for the dedicated-client apply path.
		FCartographBuildingStore* LocalStore = nullptr;
		FCartographSpatialGrid* LocalGrid = nullptr;
		FCartographTileManager* LocalTileManager = nullptr;

		// Per-tile handle membership (for clear-then-apply + LRU eviction).
		TMap<FTileId, TArray<FLocalEntry>> LocalTileHandles;
		TArray<FTileId> LocalTileLRU;                // front == most-recently-used

		// ---- Server side ----
		FCartographMapReplicator* ServerReplicator = nullptr;
		FBox2f ServerClientAoI = FBox2f(ForceInit);
		TArray<FTileId> InFlightRing;                // ordered, distance-first
		// Last tile version this client has been SENT (blob or ping), sized
		// TILE_COUNT. PushPendingDeltas diffs the live tile versions against this
		// to derive the per-tick delta set with NO external queue (stays within the
		// frozen contract - see contractDeviations). 0 == never sent.
		TArray<FTileVersion> ServerLastSentVersions;
	};

	TMap<const UCartographMapReplicationComponent*, TSharedPtr<FComponentState>>& ComponentStates()
	{
		static TMap<const UCartographMapReplicationComponent*, TSharedPtr<FComponentState>> Map;
		return Map;
	}

	FComponentState& GetComponentState(const UCartographMapReplicationComponent* Self)
	{
		TSharedPtr<FComponentState>& State = ComponentStates().FindOrAdd(Self);
		if (!State.IsValid())
		{
			State = MakeShared<FComponentState>();
		}
		return *State;
	}

	// ---- Free helpers operating on component state (declared here because the
	//      frozen header exposes no private methods to host them). ----

	/** True iff a screen box is a real (non-empty, set) viewport rect. A ForceInit
	 *  FBox2f is inverted (Min > Max), which we treat as "no AoI yet". */
	bool IsScreenBoxValid(const FBox2f& Box)
	{
		return Box.Min.X < Box.Max.X && Box.Min.Y < Box.Max.Y;
	}

	bool IsTileInBox(FTileId Tile, const FBox2f& Box)
	{
		if (!IsScreenBoxValid(Box))
		{
			return false;
		}
		const FBox2f TileRect = FCartographTileManager::TileRect(Tile);
		return TileRect.Intersect(Box);
	}

	void ClearLocalTile(FComponentState& State, FTileId Tile)
	{
		if (!State.LocalStore || !State.LocalGrid)
		{
			return;
		}
		if (TArray<FLocalEntry>* Entries = State.LocalTileHandles.Find(Tile))
		{
			for (const FLocalEntry& Entry : *Entries)
			{
				// Remove from the grid using the SAME box used at Insert (grid
				// contract). Then free the store slot + its side-table extra.
				State.LocalGrid->Remove(Entry.Handle, Entry.Box);
				State.LocalStore->Remove(Entry.Handle);
			}
			Entries->Reset();
		}
	}

	void TouchTileLRU(FComponentState& State, FTileId Tile)
	{
		State.LocalTileLRU.Remove(Tile);
		State.LocalTileLRU.Insert(Tile, 0);
	}

	void EvictTilesOutsideAoI(FComponentState& State)
	{
		for (int32 Index = State.LocalTileLRU.Num() - 1; Index >= 0; --Index)
		{
			const FTileId Tile = State.LocalTileLRU[Index];
			if (!IsTileInBox(Tile, State.LocalAoI))
			{
				ClearLocalTile(State, Tile);
				State.LocalTileHandles.Remove(Tile);
				State.LocalTileLRU.RemoveAt(Index, EAllowShrinking::No);
				// A tile we evicted may re-enter the AoI; clear its cached version so
				// it is re-pulled rather than skipped as "already current".
				if (State.ClientCachedVersions.IsValidIndex((int32)Tile))
				{
					State.ClientCachedVersions[(int32)Tile] = 0;
				}
			}
		}
	}

	void DrainInFlightRing(FComponentState& State)
	{
		UReliableMessagingPlayerComponent* Transport = State.Transport.Get();
		if (!State.ServerReplicator || !Transport || State.InFlightRing.Num() == 0)
		{
			return;
		}

		// Lazily size the last-sent-version tracking the first time we stream.
		if (State.ServerLastSentVersions.Num() != TILE_COUNT)
		{
			State.ServerLastSentVersions.SetNumZeroed(TILE_COUNT);
		}

		// Send up to RingDrainPerTick ordered tiles. The shared TSharedRef snapshot
		// keeps the SOURCE single-copy across all joiners; SendTaggedMessage copies
		// the blob bytes by value once per send (verified by-value signature), and
		// the shallow window bounds how many such copies are materialized at once.
		// SPIKE(Q9): without a transport ack callback we cannot refill strictly on
		// ack, so we drain a fixed small count per tick rather than "refill the ring
		// on ack of the oldest". Validate this keeps head-of-line blocking acceptable
		// under join + belt-drag (SPEC 4.4 Q9); if not, an ack hook is required.
		// Live-tunable drain count (Phase-3 SPIKE Q9 pacing); defaults to the prior
		// constexpr InFlightRingDepth. Clamped >= 1.
		const int32 RingDrain = FMath::Max(1, CVarCartographNetRingDrainPerTick.GetValueOnGameThread());
		const int32 Window = FMath::Min(RingDrain, State.InFlightRing.Num());
		for (int32 Sent = 0; Sent < Window; ++Sent)
		{
			const FTileId Tile = State.InFlightRing[0];
			State.InFlightRing.RemoveAt(0, EAllowShrinking::No);

			const TSharedRef<const FCartographMapReplicator::FTileSnapshot> Snapshot = State.ServerReplicator->GetTileSnapshot(Tile);

			// Copy the immutable shared blob into the by-value send buffer. This is
			// the single per-connection materialization the ring bounds.
			TArray<uint8> SendBuffer = *Snapshot->Blob;
			const int32 BlobBytes = SendBuffer.Num();
			Transport->SendTaggedMessage(CartographNetTags::Tile(), MoveTemp(SendBuffer));
			CARTO_NET_LOG("DrainInFlightRing: sent tile %u (%d bytes); ring remaining %d", (uint32)Tile, BlobBytes, State.InFlightRing.Num());

			// Record what version this client now holds so PushPendingDeltas can diff
			// against the live version next tick without an external queue.
			if (State.ServerLastSentVersions.IsValidIndex((int32)Tile))
			{
				State.ServerLastSentVersions[(int32)Tile] = Snapshot->Version;
			}
		}
	}
}


// =============================================================================
// FCartographMapReplicator - server-side per-tile snapshot authority.
// =============================================================================

void FCartographMapReplicator::Initialize(FCartographBuildingStore* InStore, FCartographSpatialGrid* InGrid, FCartographTileManager* InTileManager)
{
	FReplicatorState& State = GetReplicatorState(this);
	State.Store = InStore;
	State.Grid = InGrid;
	State.TileManager = InTileManager;

	// Lazy snapshot table sized to the full tile grid; entries are built on
	// demand by GetTileSnapshot / RepackTile so an untouched session pays nothing.
	State.Snapshots.Empty();
	State.Snapshots.SetNum(TILE_COUNT);
}

void FCartographMapReplicator::RepackTile(FTileId Tile)
{
	FReplicatorState& State = GetReplicatorState(this);
	if (!State.Store || !State.Grid || !State.TileManager)
	{
		CARTO_NET_LOG_ERROR("RepackTile called before Initialize");
		return;
	}
	if (Tile >= (FTileId)TILE_COUNT)
	{
		return;
	}

	// ---- GAME-THREAD snapshot (SPEC 5 step 2) ----
	// Workers cannot read live engine arrays or the NotThreadSafe store mid-pass,
	// so we copy out exactly the slim columns this tile covers FIRST, on the game
	// thread, into an immutable POD payload the worker owns. This is O(occupancy
	// of the tile), not the whole factory.
	const FBox2f WorldBox = TileWorldBox(Tile);
	const FTileVersion Version = State.TileManager->GetVersion(Tile);

	// Gather candidate handles from the grid (cell-granular; may include
	// duplicates + false positives). De-dup and refine against the record's own
	// screen-space tile membership so a building straddling several tiles is
	// packed only into the tiles it actually anchors in.
	TArray<FBuildingHandle> Candidates;
	State.Grid->QueryRect(WorldBox, Candidates);

	TSet<FBuildingHandle> Seen;
	Seen.Reserve(Candidates.Num());

	// The POD snapshot the worker serializes. Plain values only - no engine refs.
	TArray<FWireRecord> Packed;
	Packed.Reserve(FMath::Min(Candidates.Num(), CartographNet::MaxRecordsPerTileBlob));

	for (const FBuildingHandle Handle : Candidates)
	{
		if (Seen.Contains(Handle))
		{
			continue;
		}
		Seen.Add(Handle);

		const FBuildingRecord* Record = State.Store->Find(Handle);
		if (!Record)
		{
			continue;  // stale handle (concurrent remove) - skip
		}

		// Confirm this record actually anchors in this tile (membership). The grid
		// is world-cell-granular; the tile is screen-pixel-granular.
		const FVector2f Anchor = CartographCoords::WorldToScreen(Record->Pos);
		if (CartographCoords::TileIdFromScreen(Anchor) != Tile)
		{
			continue;
		}

		if (Packed.Num() >= CartographNet::MaxRecordsPerTileBlob)
		{
			CARTO_NET_LOG_WARNING("Tile %u exceeded MaxRecordsPerTileBlob (%d); truncating blob", Tile, CartographNet::MaxRecordsPerTileBlob);
			break;
		}

		FWireRecord WireRecord;
		WireRecord.Record = *Record;
		switch (Record->Type)
		{
		case EBuildingDrawType::Spline:
			if (Record->ExtraIndex != (uint32)INDEX_NONE)
			{
				WireRecord.Spline = State.Store->GetSplineExtra(Record->ExtraIndex);
			}
			break;
		case EBuildingDrawType::Wire:
			if (Record->ExtraIndex != (uint32)INDEX_NONE)
			{
				WireRecord.Wire = State.Store->GetWireExtra(Record->ExtraIndex);
			}
			break;
		case EBuildingDrawType::Beam:
			if (Record->ExtraIndex != (uint32)INDEX_NONE)
			{
				WireRecord.Beam = State.Store->GetBeamExtra(Record->ExtraIndex);
			}
			break;
		default:
			break;
		}
		Packed.Add(MoveTemp(WireRecord));
	}

	// ---- Serialize (SPEC 5 step 2) ----
	// The byte packing is intentionally done INLINE on the game thread here, NOT on a
	// UE::Tasks worker. The previous design launched a task and then immediately blocked
	// on GetResult() on the game thread, which is a synchronous dispatch-and-wait: it
	// adds task-graph round-trip latency on the hot net-tick path (RepackTile is reached
	// lazily from GetTileSnapshot -> DrainInFlightRing, once per AoI tile on join) for
	// ZERO concurrency gain. Per-tile occupancy is small and the framing is trivial, so
	// inline serialization is strictly cheaper than dispatch+block. If profiling later
	// shows a tile blob heavy enough to matter, convert this to a fire-and-forget task
	// whose game-thread continuation publishes the snapshot (and have GetTileSnapshot
	// return the current, possibly-stale snapshot rather than forcing a sync repack) -
	// but do NOT reintroduce a GetResult() block on the game thread.
	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes);

		uint8 Proto = CartographNet::ProtocolVersion;
		uint32 TileId = (uint32)Tile;
		uint64 TileVersion = (uint64)Version;
		uint32 RecordCount = (uint32)Packed.Num();

		Writer << Proto;
		Writer << TileId;
		Writer << TileVersion;
		Writer << RecordCount;

		for (const FWireRecord& WireRecord : Packed)
		{
			WriteRecord(Writer, WireRecord);
		}
	}

	const TSharedRef<const TArray<uint8>> Blob = MakeShared<const TArray<uint8>>(MoveTemp(Bytes));

	TSharedRef<FTileSnapshot> NewSnapshot = MakeShared<FTileSnapshot>();
	NewSnapshot->Version = Version;
	NewSnapshot->Blob = Blob;

	// Atomic pointer swap (game thread). Joiners pinning the old snapshot keep their
	// ref alive until they release it - the swap never invalidates an in-flight send.
	State.Snapshots[Tile] = NewSnapshot;
}

TSharedRef<const FCartographMapReplicator::FTileSnapshot> FCartographMapReplicator::GetTileSnapshot(FTileId Tile)
{
	FReplicatorState& State = GetReplicatorState(this);
	if (Tile >= (FTileId)TILE_COUNT || !State.TileManager || !State.Snapshots.IsValidIndex((int32)Tile))
	{
		// Degenerate request - hand back an empty, well-formed snapshot so callers
		// never null-deref (legacy #9 was three unchecked Find()s).
		return MakeShared<const FTileSnapshot>();
	}

	const TSharedPtr<const FTileSnapshot>& Existing = State.Snapshots[Tile];
	if (Existing.IsValid() && Existing->Version == State.TileManager->GetVersion(Tile))
	{
		return Existing.ToSharedRef();
	}

	// Lazily (re)build for this tile, then return the fresh shared snapshot.
	RepackTile(Tile);
	const TSharedPtr<const FTileSnapshot>& Built = State.Snapshots[Tile];
	if (Built.IsValid())
	{
		return Built.ToSharedRef();
	}
	return MakeShared<const FTileSnapshot>();
}

void FCartographMapReplicator::BuildManifest(const FBox2f& ScreenViewportBox, TArray<FTileId>& OutTiles, TArray<FTileVersion>& OutVersions) const
{
	OutTiles.Reset();
	OutVersions.Reset();

	const FReplicatorState& State = GetReplicatorState(this);
	if (!State.TileManager)
	{
		return;
	}

	// All tiles overlapping the viewport. App-level distance priority: order by
	// tile-grid distance from the viewport center so the FIFO transport queue
	// streams the most-visible tiles first (SPEC 4.4 Q9 - the queue is not
	// priority-sorted, so the ordering must be imposed here at enqueue).
	TArray<FTileId> Tiles;
	FCartographTileManager::GetTilesForBox(ScreenViewportBox, Tiles);

	const FVector2f CenterTile = ScreenBoxCenterTile(ScreenViewportBox);
	Tiles.Sort([CenterTile](const FTileId A, const FTileId B)
	{
		int32 AX, AY, BX, BY;
		CartographCoords::TileXY(A, AX, AY);
		CartographCoords::TileXY(B, BX, BY);
		const float DA = FVector2f((float)AX - CenterTile.X, (float)AY - CenterTile.Y).SizeSquared();
		const float DB = FVector2f((float)BX - CenterTile.X, (float)BY - CenterTile.Y).SizeSquared();
		return DA < DB;
	});

	OutTiles.Reserve(Tiles.Num());
	OutVersions.Reserve(Tiles.Num());
	for (const FTileId Tile : Tiles)
	{
		OutTiles.Add(Tile);
		OutVersions.Add(State.TileManager->GetVersion(Tile));
	}
}


// =============================================================================
// UCartographMapReplicationComponent - per-PlayerController transport driver.
// =============================================================================

UCartographMapReplicationComponent::UCartographMapReplicationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;  // driven by net-tick / events, not Tick
	SetIsReplicatedByDefault(false);            // no replicated UPROPERTYs; the transport is the channel
}

void UCartographMapReplicationComponent::BeginPlay()
{
	Super::BeginPlay();
	InitializeTransport();
}

void UCartographMapReplicationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Disconnect teardown is owned by the ReliableMessaging component itself; we
	// only drop our non-owning pointer + per-client state so a late
	// PushPendingDeltas cannot touch a torn-down client (kills the legacy
	// timeout-race null-deref by construction). Then erase the side-stored state.
	ComponentStates().Remove(this);

	Super::EndPlay(EndPlayReason);
}

void UCartographMapReplicationComponent::InitializeTransport()
{
	FComponentState& State = GetComponentState(this);

	// On a listen host / single-player this component is INERT: the host reads its
	// own indices directly and there is NO network path (SPEC 4.5). Detect that
	// and early-out so no transport is resolved and no handlers are registered.
	{
		const UWorld* World = GetWorld();
		const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
		if (NetMode == NM_Standalone || NetMode == NM_ListenServer)
		{
			CARTO_NET_LOG("Replication component inert (listen host / single-player); no network path");
			return;
		}
	}

	APlayerController* OwningPC = GetTypedOuter<APlayerController>();  // Within=PlayerController
	if (!OwningPC)
	{
		CARTO_NET_LOG_ERROR("Replication component has no owning PlayerController");
		return;
	}

	// SPIKE(Q1): reuse the game's auto-attached UReliableMessagingPlayerComponent
	// via GetFromPlayer. This assumes (a) the component already exists on the PC,
	// (b) its transport reports Connected without the mod driving the handshake,
	// and (c) a mod-owned Cartograph.Net.* tag can be registered without colliding
	// with CSS's FGReliableMessagingTags. NONE of this is verified in the repo and
	// it GATES all of Phase 0 (SPEC 4.4). If GetFromPlayer returns null, the mod
	// must instead AddComponent + drive the protected handshake RPCs - out of
	// scope for this resolution-only path.
	UReliableMessagingPlayerComponent* Transport = UReliableMessagingPlayerComponent::GetFromPlayer(OwningPC);
	if (!Transport)
	{
		CARTO_NET_LOG_ERROR("UReliableMessagingPlayerComponent::GetFromPlayer returned null - the game did not auto-attach the transport (SPIKE Q1). Cartograph net path disabled for this PC.");
		return;
	}
	State.Transport = Transport;

	// SPIKE(Q1) PROBE: log whether each mod-owned native tag resolves BY NAME via the
	// registry on this peer - the ReliableMessaging protocol reader rejects a tag whose
	// name does not resolve on the receiving side, so a NO here on either end explains a
	// silently-dropped message (tag-rejection, distinct from a dead transport).
	{
		const TCHAR* Role = OwningPC->HasAuthority() ? TEXT("server") : TEXT("client");
		const FGameplayTag Tags[] = { CartographNetTags::Manifest(), CartographNetTags::Tile(), CartographNetTags::Version(), AoITag() };
		for (const FGameplayTag& T : Tags)
		{
			const FGameplayTag Resolved = FGameplayTag::RequestGameplayTag(T.GetTagName(), /*ErrorIfNotFound*/ false);
			CARTO_NET_LOG("Tag '%s' resolves=%s (role=%s)", *T.GetTagName().ToString(), Resolved.IsValid() ? TEXT("YES") : TEXT("NO"), Role);
		}
	}

	// Register the mod-owned handlers. The SAME component carries all four fixed
	// tags; the role (client vs server) determines which fire. Binding both sides'
	// handlers is harmless because the wrong-role tags never arrive.
	// SPIKE(Q1): verify RegisterTaggedMessageHandler accepts a mod-owned native tag
	// and does not collide with the game's reserved internal tag ids.
	Transport->RegisterTaggedMessageHandler(
		CartographNetTags::Tile(),
		UReliableMessagingPlayerComponent::FOnBulkDataReplicationPayloadReceived::CreateUObject(this, &UCartographMapReplicationComponent::OnTileReceived));

	Transport->RegisterTaggedMessageHandler(
		CartographNetTags::Manifest(),
		UReliableMessagingPlayerComponent::FOnBulkDataReplicationPayloadReceived::CreateUObject(this, &UCartographMapReplicationComponent::OnManifestReceived));

	Transport->RegisterTaggedMessageHandler(
		CartographNetTags::Version(),
		UReliableMessagingPlayerComponent::FOnBulkDataReplicationPayloadReceived::CreateUObject(this, &UCartographMapReplicationComponent::OnVersionReceived));

	Transport->RegisterTaggedMessageHandler(
		AoITag(),
		UReliableMessagingPlayerComponent::FOnBulkDataReplicationPayloadReceived::CreateUObject(this, &UCartographMapReplicationComponent::OnAoIRequested));

	// Client-side per-tile cached versions are sized to the full grid (O(1) version
	// check on chunk-load; SPEC 4.5). Zero == never seen.
	if (!OwningPC->HasAuthority())
	{
		State.ClientCachedVersions.SetNumZeroed(TILE_COUNT);
		State.ClientPendingRepull.Init(false, TILE_COUNT);
	}

	CARTO_NET_LOG("Replication transport initialized for PC '%s' (role=%s)", *OwningPC->GetName(), OwningPC->HasAuthority() ? TEXT("server") : TEXT("client"));
}


// ---- Client side ----

void UCartographMapReplicationComponent::RequestAoI(const FBox2f& ScreenViewportBox)
{
	FComponentState& State = GetComponentState(this);
	State.LocalAoI = ScreenViewportBox;

	UReliableMessagingPlayerComponent* Transport = State.Transport.Get();

	// Inert path: no transport (listen host / SP, or resolution failed) -> nothing
	// to send; the host renders from its own indices.
	if (!Transport)
	{
		CARTO_NET_LOG_WARNING("RequestAoI: no transport bound (listen host/SP, or GetFromPlayer failed - SPIKE Q1) - no-op");
		return;
	}

	// Send the viewport AoI to the server. The server replies with a manifest, then
	// streams snapshots in distance order (SPEC 5 join flow).
	TArray<uint8> Payload;
	FMemoryWriter Writer(Payload);
	uint8 Proto = CartographNet::ProtocolVersion;
	Writer << Proto;
	float MinX = ScreenViewportBox.Min.X;
	float MinY = ScreenViewportBox.Min.Y;
	float MaxX = ScreenViewportBox.Max.X;
	float MaxY = ScreenViewportBox.Max.Y;
	Writer << MinX;
	Writer << MinY;
	Writer << MaxX;
	Writer << MaxY;

	// SendTaggedMessage takes the payload BY VALUE (verified). This AoI request is
	// tiny (17 bytes), so the by-value cost is negligible.
	Transport->SendTaggedMessage(AoITag(), MoveTemp(Payload));
	CARTO_NET_LOG("RequestAoI: handed whole-world/viewport AoI to transport [%.0f,%.0f]-[%.0f,%.0f] (separates 'mod never sent' from 'sent but lost')",
		ScreenViewportBox.Min.X, ScreenViewportBox.Min.Y, ScreenViewportBox.Max.X, ScreenViewportBox.Max.Y);
}

void UCartographMapReplicationComponent::OnTileReceived(FGameplayTag Tag, TArray<uint8>&& Payload)
{
	FComponentState& State = GetComponentState(this);

	// SPIKE(Q1) round-trip PASS marker (client side): a tile blob actually arrived.
	CARTO_NET_LOG("OnTileReceived: tile blob arrived on client (%d bytes) - transport round-trip CONFIRMED", Payload.Num());

	// The handler receives the completed tile blob by RVALUE-REF (move, per SPEC 5
	// step 4) - we never copy the inbound bytes.
	FMemoryReader Reader(Payload);

	uint8 Proto = 0;
	Reader << Proto;
	if (Reader.IsError() || Proto != CartographNet::ProtocolVersion)
	{
		CARTO_NET_LOG_WARNING("Dropped tile blob: protocol mismatch (got %u, want %u)", Proto, CartographNet::ProtocolVersion);
		return;
	}

	uint32 TileId = 0;
	uint64 TileVersion = 0;
	uint32 RecordCount = 0;
	Reader << TileId;
	Reader << TileVersion;
	Reader << RecordCount;
	if (Reader.IsError() || TileId >= (uint32)TILE_COUNT || RecordCount > (uint32)CartographNet::MaxRecordsPerTileBlob)
	{
		CARTO_NET_LOG_WARNING("Dropped malformed tile blob (tile=%u count=%u)", TileId, RecordCount);
		return;
	}

	const FTileId Tile = (FTileId)TileId;

	// Idempotent: if we already hold this exact-or-newer version for the tile, the
	// apply is a no-op (reconnect/late-join determinism, SPEC 4.4). The version is a
	// monotonic counter, so an older-or-equal version is never re-applied.
	if (State.ClientCachedVersions.IsValidIndex((int32)Tile)
		&& State.ClientCachedVersions[(int32)Tile] != 0
		&& (FTileVersion)TileVersion <= State.ClientCachedVersions[(int32)Tile])
	{
		return;
	}

	// Decode all records first (validate the whole payload before mutating local
	// state) so a truncated blob never leaves the tile half-applied.
	TArray<FWireRecord> Decoded;
	Decoded.Reserve((int32)RecordCount);
	for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
	{
		FWireRecord WireRecord;
		if (!ReadRecord(Reader, WireRecord))
		{
			CARTO_NET_LOG_WARNING("Dropped tile blob: malformed record %u/%u in tile %u", RecordIndex, RecordCount, TileId);
			return;
		}
		Decoded.Add(MoveTemp(WireRecord));
	}

	// CLEAR-THEN-APPLY into the local slim store + grid (SPEC 5 step 4). The
	// dedicated client owns a local store/grid the host does not; if they are not
	// bound (e.g. inert path raced an EndPlay) we drop the blob rather than crash.
	if (!State.LocalStore || !State.LocalGrid)
	{
		CARTO_NET_LOG_WARNING("Tile blob received but local store/grid not bound; dropping tile %u", TileId);
		return;
	}

	ClearLocalTile(State, Tile);

	TArray<FLocalEntry>& TileEntries = State.LocalTileHandles.FindOrAdd(Tile);
	TileEntries.Reset();

	for (FWireRecord& WireRecord : Decoded)
	{
		// Resolve the typed extra into the LOCAL store's side-tables and patch the
		// record's ExtraIndex to the local index (the server's ExtraIndex indexed
		// the server's tables and is meaningless on the client).
		switch (WireRecord.Record.Type)
		{
		case EBuildingDrawType::Spline:
			WireRecord.Record.ExtraIndex = State.LocalStore->AddSplineExtra(WireRecord.Spline);
			break;
		case EBuildingDrawType::Wire:
			WireRecord.Record.ExtraIndex = State.LocalStore->AddWireExtra(WireRecord.Wire);
			break;
		case EBuildingDrawType::Beam:
			WireRecord.Record.ExtraIndex = State.LocalStore->AddBeamExtra(WireRecord.Beam);
			break;
		default:
			WireRecord.Record.ExtraIndex = (uint32)INDEX_NONE;
			break;
		}

		const FBuildingHandle Handle = State.LocalStore->Add(WireRecord.Record);

		// Insert into the local grid using the record's WORLD-space anchor box. We
		// do not have the full draw VisualBox here without ComputeDrawGeometry; a
		// point box at the anchor is sufficient for AoI culling on the client (the
		// local compositor refines exact coverage at draw). Store the SAME box for
		// the later eviction Remove (grid contract: same box to Remove as Insert).
		const FVector2f WorldAnchor(WireRecord.Record.Pos.X, WireRecord.Record.Pos.Y);
		const FBox2f WorldBox(WorldAnchor, WorldAnchor);
		State.LocalGrid->Insert(Handle, WorldBox);

		TileEntries.Add(FLocalEntry{ Handle, WorldBox });
	}

	// Record the version (O(1) chunk-load check, SPEC 4.5) and clear any pending
	// re-pull flag for this tile.
	if (State.ClientCachedVersions.IsValidIndex((int32)Tile))
	{
		State.ClientCachedVersions[(int32)Tile] = (FTileVersion)TileVersion;
	}
	if (State.ClientPendingRepull.IsValidIndex((int32)Tile))
	{
		State.ClientPendingRepull[(int32)Tile] = false;
	}

	// Mark the tile dirty for LOCAL render (the never-cancelled compositor drains it
	// under the frame budget).
	if (State.LocalTileManager)
	{
		State.LocalTileManager->MarkDirty(Tile);
	}

	// LRU: this tile was just touched - move it to the most-recent end, then evict
	// any tile that has fallen outside the current AoI (bounded working set).
	TouchTileLRU(State, Tile);
	EvictTilesOutsideAoI(State);
}

void UCartographMapReplicationComponent::OnManifestReceived(FGameplayTag Tag, TArray<uint8>&& Payload)
{
	FComponentState& State = GetComponentState(this);

	FMemoryReader Reader(Payload);
	TArray<FTileId> Tiles;
	TArray<FTileVersion> Versions;
	if (!ReadPairTable(Reader, Tiles, Versions))
	{
		CARTO_NET_LOG_WARNING("Dropped malformed manifest");
		return;
	}

	// Diff against locally cached versions; flag tiles we lack or whose version is
	// stale. The manifest arrived distance-ordered (BuildManifest); the server
	// streams those AoI tiles unprompted after the manifest, so here we only record
	// what is still outstanding so a duplicate inbound tile blob is recognized as
	// already-current, and flag missing tiles for a lazy re-pull if they never come.
	for (int32 PairIndex = 0; PairIndex < Tiles.Num(); ++PairIndex)
	{
		const FTileId Tile = Tiles[PairIndex];
		const FTileVersion ServerVersion = Versions[PairIndex];
		if (!State.ClientCachedVersions.IsValidIndex((int32)Tile))
		{
			continue;
		}
		const FTileVersion Cached = State.ClientCachedVersions[(int32)Tile];
		if (Cached == 0 || Cached < ServerVersion)
		{
			if (State.ClientPendingRepull.IsValidIndex((int32)Tile))
			{
				State.ClientPendingRepull[(int32)Tile] = true;
			}
		}
	}
}

void UCartographMapReplicationComponent::OnVersionReceived(FGameplayTag Tag, TArray<uint8>&& Payload)
{
	FComponentState& State = GetComponentState(this);

	FMemoryReader Reader(Payload);
	TArray<FTileId> Tiles;
	TArray<FTileVersion> Versions;
	if (!ReadPairTable(Reader, Tiles, Versions))
	{
		CARTO_NET_LOG_WARNING("Dropped malformed version ping");
		return;
	}

	// O(1) per tile: mark named tiles as needing a lazy re-pull when they next enter
	// the viewport. NEVER a recompute here (SPEC 4.5).
	bool bAnyInViewNeedsPull = false;
	for (int32 PairIndex = 0; PairIndex < Tiles.Num(); ++PairIndex)
	{
		const FTileId Tile = Tiles[PairIndex];
		const FTileVersion ServerVersion = Versions[PairIndex];
		if (!State.ClientCachedVersions.IsValidIndex((int32)Tile))
		{
			continue;
		}
		if (State.ClientCachedVersions[(int32)Tile] < ServerVersion)
		{
			if (State.ClientPendingRepull.IsValidIndex((int32)Tile))
			{
				State.ClientPendingRepull[(int32)Tile] = true;
			}
			// If the tile is currently inside the viewport, it needs a fresh blob now.
			if (IsTileInBox(Tile, State.LocalAoI))
			{
				bAnyInViewNeedsPull = true;
			}
		}
	}

	// Coalesce: a single re-request of the current AoI causes the server to
	// re-stream every in-view tile whose version we lack (avoids one round-trip per
	// tile). Far-away pings only set the lazy flag, consumed when the AoI next moves.
	if (bAnyInViewNeedsPull)
	{
		RequestAoI(State.LocalAoI);
	}
}


// ---- Server side ----

void UCartographMapReplicationComponent::BindServerReplicator(FCartographMapReplicator* InReplicator)
{
	FComponentState& State = GetComponentState(this);
	State.ServerReplicator = InReplicator;
}

void UCartographMapReplicationComponent::OnAoIRequested(FGameplayTag Tag, TArray<uint8>&& Payload)
{
	FComponentState& State = GetComponentState(this);
	UReliableMessagingPlayerComponent* Transport = State.Transport.Get();

	// SPIKE(Q1) round-trip marker (server side): the client's AoI request reached us.
	CARTO_NET_LOG("OnAoIRequested: AoI request reached server (%d bytes) - client->server leg CONFIRMED", Payload.Num());

	if (!State.ServerReplicator || !Transport)
	{
		CARTO_NET_LOG_WARNING("AoI request received with no bound server replicator / transport");
		return;
	}

	FMemoryReader Reader(Payload);
	uint8 Proto = 0;
	Reader << Proto;
	if (Reader.IsError() || Proto != CartographNet::ProtocolVersion)
	{
		CARTO_NET_LOG_WARNING("Dropped AoI request: protocol mismatch (got %u)", Proto);
		return;
	}

	float MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	Reader << MinX;
	Reader << MinY;
	Reader << MaxX;
	Reader << MaxY;
	if (Reader.IsError())
	{
		CARTO_NET_LOG_WARNING("Dropped malformed AoI request");
		return;
	}

	State.ServerClientAoI = FBox2f(FVector2f(MinX, MinY), FVector2f(MaxX, MaxY));

	// 1) Build + send the manifest (tile ids + versions), distance-ordered.
	TArray<FTileId> ManifestTiles;
	TArray<FTileVersion> ManifestVersions;
	State.ServerReplicator->BuildManifest(State.ServerClientAoI, ManifestTiles, ManifestVersions);
	CARTO_NET_LOG("OnAoIRequested: built manifest of %d populated tiles; sending manifest + seeding distance-ordered ring", ManifestTiles.Num());

	{
		TArray<uint8> ManifestPayload;
		FMemoryWriter Writer(ManifestPayload);
		WritePairTable(Writer, ManifestTiles, ManifestVersions);
		// SPIKE(Q1): handshake-before-bulk - the manifest send assumes the transport
		// is already Connected. The SPEC requires completing the handshake BEFORE
		// enqueuing bulk, but ReliableMessaging exposes no public "is connected"
		// query on the component, only PayloadsPendingConnection buffering. The
		// manifest is small, so buffering it pre-Connected is bounded; the bulk tile
		// ring (below) is what must stay shallow.
		Transport->SendTaggedMessage(CartographNetTags::Manifest(), MoveTemp(ManifestPayload));
	}

	// 2) Seed the per-client in-flight ring with the AoI tiles in distance order.
	//    We do NOT enqueue all of them into the transport at once (FIFO head-of-line
	//    + by-value/pending-connection amplification, SPEC 4.4 Q9). The ring holds
	//    the ORDERED tile list; PushPendingDeltas drains a shallow window per tick.
	State.InFlightRing.Reset();
	State.InFlightRing.Append(ManifestTiles);

	// Kick the first window immediately so the client starts rendering promptly.
	DrainInFlightRing(State);
}

void UCartographMapReplicationComponent::PushPendingDeltas()
{
	FComponentState& State = GetComponentState(this);
	UReliableMessagingPlayerComponent* Transport = State.Transport.Get();

	if (!State.ServerReplicator || !Transport)
	{
		return;
	}

	// Derive the per-tick delta set INTERNALLY (no external queue, so the frozen
	// contract surface is sufficient - see contractDeviations). Compare the live
	// per-tile versions for THIS client's AoI against the last version we sent it.
	// Only tiles in the client's AoI are considered: a far edit bumps its own tile's
	// version but, being outside the AoI, is never enumerated here and so never
	// costs this client bandwidth (SPEC 4.4: deltas to AoI clients only).
	//
	// O(1)-amortized intent: BuildManifest is O(tiles-in-AoI), which is a small
	// fixed viewport, NOT the factory. The version reads are O(1) each.
	if (State.ServerLastSentVersions.Num() == TILE_COUNT && IsScreenBoxValid(State.ServerClientAoI))
	{
		TArray<FTileId> AoITiles;
		TArray<FTileVersion> AoIVersions;
		State.ServerReplicator->BuildManifest(State.ServerClientAoI, AoITiles, AoIVersions);

		TArray<FTileId> ChangedTiles;
		TArray<FTileVersion> ChangedVersions;
		for (int32 Index = 0; Index < AoITiles.Num(); ++Index)
		{
			const FTileId Tile = AoITiles[Index];
			const FTileVersion LiveVersion = AoIVersions[Index];
			if (!State.ServerLastSentVersions.IsValidIndex((int32)Tile))
			{
				continue;
			}
			if (LiveVersion > State.ServerLastSentVersions[(int32)Tile])
			{
				// Coalesced: regardless of how many times the tile was dirtied since
				// the last tick, we emit ONE ping + ONE blob refresh for it.
				ChangedTiles.Add(Tile);
				ChangedVersions.Add(LiveVersion);
				// Promote the changed in-AoI tile into the ring so the client gets the
				// fresh blob (not just the ping) without a re-request round-trip.
				State.InFlightRing.AddUnique(Tile);
			}
		}

		if (ChangedTiles.Num() > 0)
		{
			// 1) Tiny version ping first (the client marks the tiles stale O(1)).
			TArray<uint8> VersionPayload;
			FMemoryWriter Writer(VersionPayload);
			WritePairTable(Writer, ChangedTiles, ChangedVersions);
			Transport->SendTaggedMessage(CartographNetTags::Version(), MoveTemp(VersionPayload));
		}
	}

	// 2) Drain a shallow window of the ordered tile ring into the transport. The
	//    drain records ServerLastSentVersions, so the next PushPendingDeltas will
	//    not re-emit a tile we just streamed (idempotent convergence).
	DrainInFlightRing(State);
}


// =============================================================================
// Client-store binding (contract gap - see contractDeviations).
//
// The frozen header declares no entry point to bind the dedicated CLIENT's local
// slim store / grid / tile-manager into the component, yet its private comment
// requires "the local slim store + grid pointers for the dedicated-client apply
// path". Rather than edit the frozen header, this binding is exposed as a
// CARTOGRAPH_API free function in the CartographNet namespace; the owning
// subsystem wires it with a one-line forward declaration:
//
//   namespace CartographNet { CARTOGRAPH_API void BindClientStores(
//       class UCartographMapReplicationComponent*, class FCartographBuildingStore*,
//       class FCartographSpatialGrid*, class FCartographTileManager*); }
//
// On a listen host / SP this is never called (no client apply path). If the
// frozen header is unfrozen, fold this into a real public method.
// =============================================================================
namespace CartographNet
{
	CARTOGRAPH_API void BindClientStores(
		UCartographMapReplicationComponent* Component,
		FCartographBuildingStore* InStore,
		FCartographSpatialGrid* InGrid,
		FCartographTileManager* InTileManager)
	{
		if (!Component)
		{
			return;
		}
		FComponentState& State = GetComponentState(Component);
		State.LocalStore = InStore;
		State.LocalGrid = InGrid;
		State.LocalTileManager = InTileManager;
	}
}
