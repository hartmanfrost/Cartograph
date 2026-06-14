#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Components/ActorComponent.h"
#include "Core/CartographTypes.h"
#include "CartographMapReplicator.generated.h"

// =============================================================================
// CartographMapReplicator.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "net" agent (CartographMapReplicator.cpp).
// REPLACES UCartographRemoteCallObject + the slice RCO wholesale (SPEC 4.4).
// -----------------------------------------------------------------------------
// Per-tile, versioned, AoI-scoped delta transport over the in-repo
// ReliableMessaging plugin. The hand-rolled slice RCO bug class (#9) is DELETED
// by construction: int32 sizes / uint64 ids (no int16 overflow), per-PC
// component with disconnect teardown (no timeout-race null-deref, no per-join
// leak), no synchronous Archive << CurrentBuildingData, no reliable
// NetMulticast per build event.
//
// Transport surface used (verified, ReliableMessagingPlayerComponent.h):
//   UReliableMessagingPlayerComponent::GetFromPlayer(PC)
//   ::SendTaggedMessage(FGameplayTag, TArray<uint8>)          // BY VALUE
//   ::RegisterTaggedMessageHandler(FGameplayTag, FOnBulkDataReplicationPayloadReceived)
//   FOnBulkDataReplicationPayloadReceived = void(FGameplayTag, TArray<uint8>&&) // by rvalue-ref
//
// SPIKE(Q1): ReliableMessaging has NO mod-wiring path in the repo. Before any
// other net work, verify on a live dedicated-server connection that
// GetFromPlayer(PC) returns a component, its transport reports Connected
// without the mod driving the handshake, and a mod-owned tag can be registered
// without colliding with CSS's FGReliableMessagingTags. Gates all of Phase 0.
//
// Protocol: a SMALL FIXED gameplay-tag set (manifest / tile / version), NOT a
// tag-per-tile (tag registration costs a round-trip, SPEC 4.4).
// =============================================================================
class UReliableMessagingPlayerComponent;
class APlayerController;
class FCartographBuildingStore;
class FCartographSpatialGrid;
class FCartographTileManager;


// -----------------------------------------------------------------------------
// FCartographMapReplicator - the server-side per-tile snapshot authority.
// One instance lives on the server. Holds one shared immutable ref-counted
// snapshot per tile (TSharedRef<const TArray<uint8>>) pinned across all joiners,
// so join memory is O(joiners + AoI), not O(joiners*N) (SPEC 4.4).
// Plain C++ object owned by the server subsystem; not a UObject.
// -----------------------------------------------------------------------------
class CARTOGRAPH_API FCartographMapReplicator
{
public:
	FCartographMapReplicator() = default;
	~FCartographMapReplicator() = default;

	FCartographMapReplicator(const FCartographMapReplicator&) = delete;
	FCartographMapReplicator& operator=(const FCartographMapReplicator&) = delete;

	/** A packed per-tile slim blob plus the version it was packed at. The blob
	 *  is shared (ref-counted, immutable) across every joiner pinning this tile. */
	struct FTileSnapshot
	{
		FTileVersion Version = 0;
		TSharedRef<const TArray<uint8>> Blob = MakeShared<const TArray<uint8>>();
	};

	/** Bind the data spine. The replicator reads the store/grid via the tile
	 *  manager's tile->box mapping; it never owns them. */
	void Initialize(FCartographBuildingStore* InStore, FCartographSpatialGrid* InGrid, FCartographTileManager* InTileManager);

	/**
	 * Re-pack the slim blob for one dirty tile from a GAME-THREAD snapshot of the
	 * mod's own slim columns (snapshot first; workers can't read live engine
	 * arrays), atomically replacing the prior shared snapshot. The heavy
	 * serialize/compress may run on a UE::Tasks worker; the pointer swap is on
	 * the game thread. Big-O: O(occupancy of the tile).
	 */
	void RepackTile(FTileId Tile);

	/** Get (or lazily build) the shared snapshot for a tile. The returned ref is
	 *  safe to hand to multiple joiners; it is immutable. */
	TSharedRef<const FTileSnapshot> GetTileSnapshot(FTileId Tile);

	/** Build the AoI manifest (tile ids + versions) for a viewport, ordered by
	 *  distance from the viewport center (app-level priority over the FIFO
	 *  transport queue, SPEC 4.4 Q9). */
	void BuildManifest(const FBox2f& ScreenViewportBox, TArray<FTileId>& OutTiles, TArray<FTileVersion>& OutVersions) const;

private:
	// Implementation (owning agent): TArray<TSharedPtr<const FTileSnapshot>>
	// sized TILE_COUNT (lazy), the non-owning data-spine pointers, and the
	// off-thread repack plumbing.
};


// -----------------------------------------------------------------------------
// Fixed mod-owned gameplay tags for the protocol (SPEC 4.4). Defined in the
// .cpp; registered once. NOT a tag-per-tile.
//   Cartograph.Net.Manifest  - manifest request/response (tile ids + versions)
//   Cartograph.Net.Tile      - a single packed tile blob
//   Cartograph.Net.Version   - a version-only delta ping for AoI tiles
// -----------------------------------------------------------------------------
namespace CartographNetTags
{
	CARTOGRAPH_API FGameplayTag Manifest();
	CARTOGRAPH_API FGameplayTag Tile();
	CARTOGRAPH_API FGameplayTag Version();
}


// -----------------------------------------------------------------------------
// UCartographMapReplicationComponent - per-PlayerController driver.
// Wraps the game's UReliableMessagingPlayerComponent (reused via GetFromPlayer,
// SPIKE Q1). On the server: streams AoI tile blobs to its owning client,
// distance-ordered, with a 1-2 message in-flight window + N-deep ring (SPEC Q9).
// On the dedicated client: sends the viewport AoI, receives manifest/tile/
// version messages, applies them into the local slim store + grid, marks tiles
// dirty for local render. On a listen host / single-player this is INERT (the
// host reads its own indices directly; no network path, SPEC 4.5).
//
// Within=PlayerController mirrors the transport component's ownership model so
// GetFromPlayer resolution is symmetric.
// -----------------------------------------------------------------------------
UCLASS(Within = PlayerController)
class CARTOGRAPH_API UCartographMapReplicationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCartographMapReplicationComponent();

	/** Resolve (or attach, SPIKE Q1) this PC's UReliableMessagingPlayerComponent
	 *  and register the mod-owned manifest/tile/version handlers. */
	void InitializeTransport();

	// ---- Client side ----

	/**
	 * Send the current UI-viewport AoI to the server. The server replies with a
	 * manifest, then streams the per-tile snapshots in AoI (distance) order. The
	 * client renders tiles progressively as they arrive. Join memory O(AoI).
	 */
	void RequestAoI(const FBox2f& ScreenViewportBox);

	/** Apply a received tile blob into the local slim store + grid, validate the
	 *  1-byte protocol version + tile version, idempotently clear-then-apply,
	 *  and mark the tile dirty for local render. */
	void OnTileReceived(FGameplayTag Tag, TArray<uint8>&& Payload);

	/** Apply a received manifest: diff against locally cached versions, enqueue
	 *  pulls for tiles the client lacks (distance-ordered). */
	void OnManifestReceived(FGameplayTag Tag, TArray<uint8>&& Payload);

	/** Apply a version ping: mark the named tiles as needing a lazy re-pull when
	 *  they next enter the viewport (O(1), never a recompute). */
	void OnVersionReceived(FGameplayTag Tag, TArray<uint8>&& Payload);

	// ---- Server side ----

	/** Bind the server-side snapshot authority so this component can stream
	 *  shared per-tile snapshots to its owning client. */
	void BindServerReplicator(FCartographMapReplicator* InReplicator);

	/** Handle a client's AoI request: build the manifest, then enqueue the
	 *  shared tile snapshots distance-ordered with a bounded in-flight window
	 *  (1-2 messages) + N-deep ring, handshake completed before bulk enqueue. */
	void OnAoIRequested(FGameplayTag Tag, TArray<uint8>&& Payload);

	/** Per net-tick: coalesce per-tile version bumps and push, to this client,
	 *  only deltas for tiles in its AoI. Far edits invalidate only their tile. */
	void PushPendingDeltas();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// Implementation (owning agent): a non-owning UReliableMessagingPlayerComponent*
	// (resolved via GetFromPlayer), the local AoI rect, the client-side cached
	// per-tile versions (TArray<FTileVersion> sized TILE_COUNT), the server-side
	// in-flight ring / pending-delta set, and the bound FCartographMapReplicator*.
	// The local slim store + grid pointers for the dedicated-client apply path.
};
