#pragma once

#include "CoreMinimal.h"
#include "Core/CartographTypes.h"

// =============================================================================
// CartographClassDrawTable.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "classtable" agent (CartographClassDrawTable.cpp).
// -----------------------------------------------------------------------------
// Per-class draw table, sized by DISTINCT classes (hundreds), built once at
// gather (SPEC 4.1). Maps UClass* -> uint16 ClassId -> const FClassDrawInfo&.
// Collapses the legacy four per-building pointers (Category/Layer/Spline/Wire)
// into one hash lookup per class at draw.
//
// Hosts the SHARED pure ComputeDrawGeometry used by ALL render backends. It
// MUST reproduce the legacy FBuildingData::FillInCache math pixel-identically
// (SPEC 4.1, Q12: diff-render a large save before deleting the legacy caches).
// =============================================================================
class FCartographBuildingStore;  // forward decl - side-tables resolve extras

// The implementing agent (classtable) sources its data from the legacy config
// TMaps that live on the game-instance module. Those are the authority for
// category color, icon override, size override, extra rotation, spline/wire
// data, layer data and the class-redirect map (legacy
// CartographGameInstanceModule.h fields). Rather than #include that whole heavy
// header into this contract, the parameterized Build overload takes the module
// pointer by forward-declared type (see the .cpp).
class UCartographGameInstanceModule;  // forward decl - legacy config source


class CARTOGRAPH_API FCartographClassDrawTable
{
public:
	FCartographClassDrawTable() = default;
	~FCartographClassDrawTable() = default;

	FCartographClassDrawTable(const FCartographClassDrawTable&) = delete;
	FCartographClassDrawTable& operator=(const FCartographClassDrawTable&) = delete;

	// -------------------------------------------------------------------------
	// Build. Once, at gather, from the asset registry / engine + the legacy
	// per-class config maps (category colors, icon overrides, size overrides,
	// extra rotation, spline/wire data). After Build, the table is read-only
	// and the ClassId<->UClass mapping is stable for the session.
	// -------------------------------------------------------------------------

	/**
	 * Build the table. Big-O: O(distinct classes). Assigns each registered
	 * buildable class a dense uint16 ClassId and fills its FClassDrawInfo.
	 * Idempotent: a second call rebuilds from scratch.
	 *
	 * NOTE on signature: the concrete sources (the legacy config TMaps living on
	 * the game-instance module, plus the asset registry) are passed by the
	 * implementing agent via a Build overload it adds in its own .cpp/.h section
	 * WITHOUT changing this frozen no-arg contract's existence. Callers that only
	 * need lookup/geometry depend solely on the methods below. If the agent needs
	 * a parameterized Build, it adds an overload; this no-arg form remains.
	 */
	void Build();

	/**
	 * Parameterized Build overload (classtable agent addition, permitted by the
	 * contract note above). Sources every per-class field from the legacy config
	 * maps on the game-instance module:
	 *   - ClassPtrToClassIDMap / ClassIDToClassPtrMap  (the gathered buildable set)
	 *   - BuildableClassRedirectMap                    (class redirects)
	 *   - BuildableSplineDataMap / BuildableWireDataMap (stroke color/thickness)
	 *   - BuildCategoryDataMap + per-buildable override (fill/outline color)
	 *   - BuildableIconOverrideMap                      (icon textures)
	 *   - BuildableSizeOverrideMap + CDO footprint walk (footprint cm)
	 *   - BuildableExtraRotationMap                     (extra yaw)
	 *   - the layer data caches                         (filter identity)
	 *   - ModdedBuildings + the Unspecified* fallbacks
	 *
	 * Mirrors the legacy FBuildingData::FillInCache class-classification cascade
	 * (CartographDataStructure.cpp:264-446) so a class resolves to exactly the
	 * draw type / colors / thickness it did before. Module may be null (logs and
	 * leaves the table empty).
	 */
	void Build(UCartographGameInstanceModule* Module);

	// -------------------------------------------------------------------------
	// Lookup. All O(1) after Build.
	// -------------------------------------------------------------------------

	/** Map a buildable UClass* to its dense ClassId. Returns INDEX_NONE-cast
	 *  sentinel (0xFFFF) if the class was not registered. O(1). */
	uint16 GetClassId(const UClass* BuildableClass) const;

	/** Sentinel returned by GetClassId for an unregistered class. */
	static constexpr uint16 InvalidClassId = 0xFFFF;

	/** Resolve a ClassId to its draw info. PRECONDITION: ClassId is valid
	 *  (< NumClasses, != InvalidClassId). O(1). */
	const FClassDrawInfo& GetInfo(uint16 ClassId) const;

	/** Safe form: nullptr if ClassId is out of range. O(1). */
	const FClassDrawInfo* FindInfo(uint16 ClassId) const;

	/** Number of distinct registered classes. */
	int32 NumClasses() const;

	/** Collect every distinct icon TSoftObjectPtr across the table so the caller
	 *  can PIN them before a draw pass (SPEC 4.3: never AsyncLoad mid-pass). */
	void GatherIconAssets(TArray<TSoftObjectPtr<UTexture2D>>& OutIcons) const;

	// -------------------------------------------------------------------------
	// Geometry. The shared, pure draw-math used by every backend.
	// -------------------------------------------------------------------------

	/**
	 * Compute the screen-space draw geometry for one building. PURE: depends
	 * only on the record, its class info, and the store's typed side-tables
	 * (for spline/wire/beam extras). No engine queries, no allocation beyond the
	 * spline point array. Safe to call off the game thread on an immutable
	 * snapshot.
	 *
	 * MUST reproduce the legacy FBuildingData::FillInCache + FillInVisualBoxCache
	 * results (SPEC 4.1, Q12), including:
	 *   - Rectangle corners: build the four corners at +/- half-footprint*scale,
	 *     transform by the (yaw-only) transform with scale removed, project to
	 *     screen. (legacy CartographDataStructure.cpp:417-441, 526-540)
	 *   - Icon: screen pos via CartographCoords::WorldToScreen(pos, size),
	 *     size = footprint*scale, rotation = yaw + class ExtraYaw.
	 *   - Spline/Wire/Beam: project the side-table points to screen; stroke
	 *     color/thickness from class info.
	 *   - VisualBox: union of the drawn geometry, expanded by
	 *     BOX_EXPANSION_CENTIMETERS-equivalent in screen space.
	 *
	 * @param Record   the slim record (Pos, PackedYaw, ClassId, Type, ExtraIndex)
	 * @param Info     class draw info for Record.ClassId
	 * @param Store    resolves Record.ExtraIndex against the typed side-tables
	 * @param OutGeometry  filled on success
	 * @return false if the building is not drawable (e.g. zero footprint,
	 *         zero stroke thickness) - matches legacy "don't bother" early-outs.
	 *
	 * Big-O: O(1) for Icon/Rectangle/Wire/Beam; O(SPLINE_SEGMENTS) for Spline.
	 */
	static bool ComputeDrawGeometry(
		const FBuildingRecord& Record,
		const FClassDrawInfo& Info,
		const FCartographBuildingStore& Store,
		FDrawGeometry& OutGeometry);

	// -------------------------------------------------------------------------
	// Footprint derivation (classtable agent addition; salvaged from Cartograph_2.0
	// GetBuildingBoundingBox per SPEC 8). Public+static so Phase 1 callers that
	// pre-warm the size cache (and any diff-test harness) can reuse the exact
	// derivation the table itself uses. The legacy GetBuildingSize cascade:
	//   1. BuildableSizeOverrideMap lookup (designer override / prior cache);
	//   2. CDO->GetCombinedClearanceBox() if valid (clearance footprint);
	//   3. CDO-walk fallback: union the local bounds of every SceneComponent in
	//      the BlueprintGeneratedClass ICH + SCS chain and the native CDO
	//      subobjects, up the super-class chain.
	// Returns FVector2f::ZeroVector when no footprint is derivable (legacy
	// returns FVector2D{} and the building is then not drawn).
	// -------------------------------------------------------------------------
	static FVector2f DeriveFootprintFromCDO(const UClass* BuildableClass);

private:
	// Implementation (owning agent): TMap<const UClass*, uint16> for the lookup
	// and a TArray<FClassDrawInfo> indexed by ClassId. Build sources its data
	// from the legacy config maps and CDO footprint derivation
	// (GetCombinedClearanceBox + CDO-walk fallback, legacy
	// CartographDataStructure.cpp:565-655 / SPEC salvage from Cartograph_2.0).
	TMap<const UClass*, uint16> ClassToId;
	TArray<FClassDrawInfo> Infos;
};
