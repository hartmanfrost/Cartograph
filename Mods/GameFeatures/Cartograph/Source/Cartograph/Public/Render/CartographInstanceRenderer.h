#pragma once

#include "CoreMinimal.h"
#include "Core/CartographTypes.h"
#include "CartographInstanceRenderer.generated.h"

// =============================================================================
// CartographInstanceRenderer.h - FROZEN PUBLIC API (Agent: contracts)
// Implemented by: the "instance" agent (CartographInstanceRenderer.cpp).
// PHASE 4 / C. CVar-GATED: only used when r.Cartograph.RenderBackend == 2
// (InstancedCapture). The TiledCanvas/Slate path is the always-available
// fallback (SPEC 4.2, Q5/Q6/Q7).
// -----------------------------------------------------------------------------
// GPU-instanced render store: one ULightweightHISM per class on a hidden,
// collision-disabled marker actor, captured top-down by an orthographic
// USceneCaptureComponent2D. Per-instance custom data (worldZ, layerId,
// colorIndex) drives a material clip() for Z-band + layer filtering at ZERO
// per-event CPU. Add/remove become O(1) handle ops (SetInstanceFromDataStatic /
// RemoveInstance, internal swap-patch).
//
// HARD CAVEATS folded into this contract (SPEC 4.1 refinement, Q6/Q7):
//  - FInstanceOwnerHandlePtr is TSharedPtr<..., ESPMode::NotThreadSafe>:
//    ALL handle mutation is GAME-THREAD-ONLY and UNPARALLELIZABLE. A 1M cold
//    load is a many-frame game-thread drain (benchmark before promoting over B).
//  - AbstractInstance.Build.cs pulls AkAudio/Wwise transitively; enabling it is
//    not a one-line uncomment (Phase 4 prerequisite, not a flag).
//  - The manager builds ULightweightCollisionComponent by default; disable
//    collision for a visual-only marker layer.
//  - Capture isolation (PrimitiveRenderMode=UseShowOnlyList + ShowOnlyActors +
//    GetViewOwner override) is UNVERIFIED on the CSS fork (the POC never
//    compiled). SPIKE(Q5) before committing.
//  - bEnableOrthographicTiling / NumXTiles / NumYTiles appear in zero repo C++:
//    treat as conditional upside, NOT a TDR guarantee. SPIKE(Q5). Ortho tiling
//    requires CaptureSource=SceneColor, not FinalColor.
//
// Modeled as a UObject so it can own the marker AActor + capture component and
// participate in the GC graph. UPROPERTY where reflection is needed.
// =============================================================================
class FCartographBuildingStore;
class FCartographClassDrawTable;
class AActor;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

// AbstractInstance plugin types, forward-declared so this PUBLIC header does NOT
// include InstanceData.h (which pulls AkAudio/Wwise transitively into every
// translation unit that includes us; SPEC 7 / Q6). The handle alias mirrors the
// plugin's `using FInstanceOwnerHandlePtr = TSharedPtr<FInstanceOwnershipHandle,
// ESPMode::NotThreadSafe>` (InstanceData.h:300). A TSharedPtr to a forward-
// declared pointee is a complete type for TMap value purposes; the pointee is
// only completed in the .cpp (which DOES include the plugin header). FInstanceData
// is stored behind a TSharedPtr for the same reason (avoid value-completeness of
// a USTRUCT that drags Ak headers into the public include).
struct FInstanceOwnershipHandle;
struct FInstanceData;
using FCartographInstanceHandlePtr = TSharedPtr<FInstanceOwnershipHandle, ESPMode::NotThreadSafe>;

// -----------------------------------------------------------------------------
// Per-instance custom-data float layout consumed by the marker material's
// clip() (SPEC 4.2 Phase C). The marker material is authored to read these
// indices and discard fragments outside the active Z-band / layer mask. Kept
// here so the .cpp, the material author, and any future backend agree on the
// slot meaning. NumCustomDataFloats on the FInstanceData MUST be >= this count.
// -----------------------------------------------------------------------------
namespace CartographInstanceCustomData
{
	/** World Z in cm (drives the material Z-band clip vs the SetZFilter range). */
	inline constexpr int32 WorldZ = 0;
	/** Dense layer id (drives the material per-layer visibility mask). */
	inline constexpr int32 LayerId = 1;
	/** Palette index for the fill color lookup in the marker material. */
	inline constexpr int32 ColorIndex = 2;
	/** Per-instance show flag (0 hidden, 1 shown) for global building toggle. */
	inline constexpr int32 ShowFlag = 3;

	/** Total custom-data floats per instance. */
	inline constexpr int32 NumFloats = 4;
}


UCLASS()
class CARTOGRAPH_API UCartographInstanceRenderer : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Spawn the hidden marker actor, one ULightweightHISM per class (from the
	 * class table), and the orthographic capture component. Collision disabled.
	 * SPIKE(Q5): validate capture isolation on the fork before relying on output.
	 * SPIKE(Q6): confirm the AbstractInstance/Wwise link and a no-op no-Ak manager.
	 */
	void Initialize(FCartographBuildingStore* InStore, FCartographClassDrawTable* InClassTable, UWorld* World);

	/** Tear down the marker actor + capture; release handles. */
	void Shutdown();

	// -------------------------------------------------------------------------
	// O(1) instance mutation (GAME-THREAD-ONLY; handles are NotThreadSafe).
	// Called from build hooks for backend C in place of the tile-dirty path.
	// -------------------------------------------------------------------------

	/**
	 * Add a GPU instance for Handle's record. Resolves the per-class HISM,
	 * SetInstanceFromDataStatic (O(1) swap-patch), stores the returned
	 * FInstanceOwnerHandlePtr keyed by FBuildingHandle, and writes per-instance
	 * custom data (worldZ, layerId, colorIndex). MarkDirtyDeferred-batched.
	 */
	void AddInstance(FBuildingHandle Handle, const FBuildingRecord& Record);

	/** Remove the GPU instance for Handle (O(1) swap-patch). Frees the stored
	 *  FInstanceOwnerHandlePtr. No-op if Handle has no instance. */
	void RemoveInstance(FBuildingHandle Handle);

	/** Patch the transform/custom-data for an existing instance in place (O(1)). */
	void UpdateInstance(FBuildingHandle Handle, const FBuildingRecord& Record);

	// -------------------------------------------------------------------------
	// Filtering at zero CPU (material-driven via per-instance custom data).
	// -------------------------------------------------------------------------

	/** Set the Z-band clip range on the marker material (2 scalar writes, no
	 *  per-instance work). Replaces the legacy O(N) Z re-walk entirely. */
	void SetZFilter(float MinZ, float MaxZ);

	/** Toggle a layer's visibility via the material layer mask. */
	void SetLayerVisible(FName LayerMainCategory, FName LayerSubCategory, bool bVisible);

	/** Toggle all building visibility. */
	void SetShowBuildings(bool bShow);

	// -------------------------------------------------------------------------
	// Capture.
	// -------------------------------------------------------------------------

	/** Trigger one debounced orthographic capture into the output target.
	 *  CaptureSource must be SceneColor if ortho tiling is used (SPIKE Q5). */
	void RequestCapture();

	/** The capture output the map UI samples (replaces the FCanvas atlas in C). */
	UTextureRenderTarget2D* GetCaptureTarget() const;

private:
	// Implementation (owning agent): the hidden AActor* marker, a
	// TMap<uint16 ClassId, ULightweightHISM*> per-class component map, a
	// TMap<FBuildingHandle, FInstanceOwnerHandlePtr> handle map (note: the value
	// type is NotThreadSafe - game-thread-only), the USceneCaptureComponent2D,
	// and the UTextureRenderTarget2D output. All wired behind the CVar gate.

	UPROPERTY()
	TObjectPtr<AActor> MarkerActor;

	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

	UPROPERTY()
	TObjectPtr<UTextureRenderTarget2D> CaptureTarget;

	// -------------------------------------------------------------------------
	// Owning-agent private state (the public surface above is frozen; this
	// internal layout is the agent's choice). Declared in the .generated body
	// only where reflection/GC ownership is required; pure-C++ maps otherwise.
	// -------------------------------------------------------------------------

	/** Injected, non-owning. The subsystem outlives this renderer. */
	FCartographBuildingStore* Store = nullptr;
	FCartographClassDrawTable* ClassTable = nullptr;

	/** The dynamic material instance driving the Z-band / layer / palette clip().
	 *  Two scalar writes per SetZFilter; a layer-mask scalar per SetLayerVisible.
	 *  Shared across every per-class ULightweightHISM via OverrideMaterials. */
	UPROPERTY()
	TObjectPtr<class UMaterialInstanceDynamic> MarkerMID;

	/** Source material the MID is created from (loaded from the mod's content). */
	UPROPERTY()
	TObjectPtr<class UMaterialInterface> MarkerBaseMaterial;

	/** The unit-quad mesh each per-class instance renders (a flat marker quad). */
	UPROPERTY()
	TObjectPtr<class UStaticMesh> MarkerMesh;

	/** Stable handle -> AbstractInstance handle. NotThreadSafe value:
	 *  game-thread-only. SPIKE(Q7): high-churn cost on a 1M cold load. */
	TMap<FBuildingHandle, FCartographInstanceHandlePtr> InstanceHandles;

	/** ClassId -> the FInstanceData template used to spawn that class's HISM.
	 *  Built once at Initialize from the class table. Held behind a TSharedPtr so
	 *  this header need not complete the Ak-pulling FInstanceData USTRUCT. */
	TMap<uint16, TSharedPtr<FInstanceData>> ClassInstanceTemplates;

	/** Dense layer id assignment so the material can mask by a small int.
	 *  {MainCategory,SubCategory} -> dense uint16, assigned at Initialize. */
	TMap<uint32, int32> LayerIdMap;

	/** Per-layer visibility (mirrors the material layer-mask state). */
	TBitArray<> LayerVisible;

	/** Whether buildings draw at all (global toggle; see SetShowBuildings). */
	bool bShowBuildings = true;

	/** Debounce: a capture is already requested for the next tick. */
	bool bCapturePending = false;

	// Internal helpers (impl-only; not part of the frozen surface).
	void BuildClassTemplates();
	void EnsureCaptureRig(UWorld* World);
	FInstanceData* FindOrAddClassTemplate(uint16 ClassId);
	void WriteCustomData(const FCartographInstanceHandlePtr& Handle, const FBuildingRecord& Record) const;
	int32 GetLayerId(FName MainCategory, FName SubCategory);
	static uint32 LayerKey(FName MainCategory, FName SubCategory);
	FTransform ComputeInstanceTransform(const FBuildingRecord& Record) const;
};
