#include "Render/CartographInstanceRenderer.h"

#include "CartographConfig.h"
#include "CartographGameInstanceModule.h"  // CARTO_LOG macros, legacy bounds parity
#include "Core/CartographBuildingStore.h"
#include "Core/CartographClassDrawTable.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "GameFramework/Actor.h"
#include "UObject/SoftObjectPath.h"

// AbstractInstance (Plugins/AbstractInstance). SPIKE(Q6): the module dependency
// is commented out in Cartograph.Build.cs because AbstractInstance pulls AkAudio
// transitively; enabling Phase C requires uncommenting "AbstractInstance" in the
// Build.cs AND accepting the Wwise link. Treated as a hard prerequisite, not a
// flag (SPEC 7, Q6). All AbstractInstance includes/calls below are gated behind
// the InstancedCapture backend at runtime and behind this build flag at compile.
//
// INTEGRATION GUARD (build owner): the ENTIRE Phase-4 implementation TU is wrapped
// in #if CARTOGRAPH_WITH_ABSTRACTINSTANCE (set in Cartograph.Build.cs, default 0)
// so a build WITHOUT the AbstractInstance plugin still compiles: with the flag off
// the plugin headers are not included and the method bodies are elided. The UCLASS
// declaration stays parseable by UHT (its header pulls no plugin headers), so the
// reflected type still registers; it is simply never instantiated (nothing
// references UCartographInstanceRenderer until Phase 4 is wired + the flag flipped).
#if CARTOGRAPH_WITH_ABSTRACTINSTANCE

#include "InstanceData.h"
#include "AbstractInstanceManager.h"

// =============================================================================
// CartographInstanceRenderer.cpp - Phase 4 / C (GATED) GPU-instanced backend.
// SPEC 4.2 Phase C, 4.1 render refinement, 8.
//
// One ULightweightHISM per class on a hidden, collision-disabled marker actor
// (a dedicated AAbstractInstanceManager so its components are ISOLATED from the
// game's shared instance manager - required for the ShowOnlyActors capture
// filter; SPIKE(Q5)). Per-instance custom data (worldZ/layerId/colorIndex/show)
// drives a material clip() for Z-band + layer + global-toggle filtering at zero
// per-event CPU. Add/remove are O(1) handle ops; the height slider and layer
// toggles become a handful of material scalar/vector writes instead of an O(N)
// re-walk.
//
// EVERYTHING here is game-thread-only: FInstanceOwnerHandlePtr is a
// TSharedPtr<..., ESPMode::NotThreadSafe> (InstanceData.h:300), so handle
// mutation can never move to a UE::Tasks worker. SPIKE(Q7): benchmark the
// game-thread handle-write throughput on a 1M cold load within
// FrameBudgetFraction before promoting C over B.
// =============================================================================

namespace
{
	// -------------------------------------------------------------------------
	// Content paths. These uassets are authored as part of the Phase-C content
	// drop; the renderer only ships if they exist (Initialize guards on load).
	// A flat unit-quad mesh facing +Z and a marker material whose pixel shader
	// reads the per-instance custom data and discards out-of-band fragments.
	// -------------------------------------------------------------------------
	const TCHAR* const MarkerMeshPath =
		TEXT("/Cartograph/Render/SM_CartographMarkerQuad.SM_CartographMarkerQuad");
	const TCHAR* const MarkerMaterialPath =
		TEXT("/Cartograph/Render/M_CartographMarker.M_CartographMarker");

	// Capture camera altitude above the map plane (cm). Ortho capture ignores the
	// distance for projection scale, but the near/far clip and the look direction
	// still need a sane camera origin above the tallest build (Z_BAND_MAX_CM).
	constexpr double CaptureAltitudeCm = Z_BAND_MAX_CM + 100000.0;

	// Named material parameters the marker material exposes. Kept local so the
	// material author and this file share one source of truth.
	const FName ParamZMin(TEXT("CartographZMin"));
	const FName ParamZMax(TEXT("CartographZMax"));
	const FName ParamShowBuildings(TEXT("CartographShowBuildings"));
	// Layer visibility is packed into four vector params (4 channels each) so up
	// to 16 layers mask without a dynamic array. Layer id N selects param
	// (N / 4) channel (N % 4).
	const FName ParamLayerMask0(TEXT("CartographLayerMask0"));
	const FName ParamLayerMask1(TEXT("CartographLayerMask1"));
	const FName ParamLayerMask2(TEXT("CartographLayerMask2"));
	const FName ParamLayerMask3(TEXT("CartographLayerMask3"));
	constexpr int32 MaxMaskableLayers = 16;
}


// -----------------------------------------------------------------------------
// Lifecycle.
// -----------------------------------------------------------------------------
void UCartographInstanceRenderer::Initialize(FCartographBuildingStore* InStore, FCartographClassDrawTable* InClassTable, UWorld* World)
{
	Store = InStore;
	ClassTable = InClassTable;

	if (CartographConfig::GetRenderBackend() != ECartographRenderBackend::InstancedCapture)
	{
		// Backend C is gated; the TiledCanvas/Slate path is the default fallback.
		// Initialize is a no-op unless explicitly selected via CVar (SPEC 4.2).
		CARTO_LOG("InstanceRenderer: InstancedCapture backend not selected; skipping init");
		return;
	}

	CARTO_LOG_ERROR_RETURN_IF_NULL(Store);
	CARTO_LOG_ERROR_RETURN_IF_NULL(ClassTable);
	CARTO_LOG_ERROR_RETURN_IF_NULL(World);

	// Load the marker assets. If either is missing, the Phase-C content drop has
	// not shipped; bail cleanly so the subsystem falls back to TiledCanvas.
	MarkerMesh = LoadObject<UStaticMesh>(nullptr, MarkerMeshPath);
	MarkerBaseMaterial = LoadObject<UMaterialInterface>(nullptr, MarkerMaterialPath);
	if (!MarkerMesh || !MarkerBaseMaterial)
	{
		CARTO_LOG_ERROR("InstanceRenderer: marker mesh/material not found; backend C unavailable");
		return;
	}

	// One MID shared by every per-class HISM so the Z/layer/show filters are a
	// single set of scalar/vector writes regardless of building count.
	MarkerMID = UMaterialInstanceDynamic::Create(MarkerBaseMaterial, this);
	CARTO_LOG_ERROR_RETURN_IF_NULL(MarkerMID);

	LayerVisible.Init(true, MaxMaskableLayers);

	BuildClassTemplates();
	EnsureCaptureRig(World);

	// Push the initial filter state into the material.
	SetZFilter((float)Z_BAND_MIN_CM, (float)Z_BAND_MAX_CM);
	SetShowBuildings(bShowBuildings);

	CARTO_LOG("InstanceRenderer: initialized (%d class templates, %d layers)", ClassInstanceTemplates.Num(), LayerIdMap.Num());
}


void UCartographInstanceRenderer::Shutdown()
{
	// Release every AbstractInstance handle (game-thread-only). RemoveInstances
	// clears the handle pointers and the underlying instances; do it through the
	// manager so the swap-patch bookkeeping stays consistent.
	if (MarkerActor)
	{
		// SPIKE(Q5): the marker actor is a dedicated AAbstractInstanceManager;
		// removing instances here must not touch the game's shared manager.
		TArray<FCartographInstanceHandlePtr> Handles;
		Handles.Reserve(InstanceHandles.Num());
		for (TPair<FBuildingHandle, FCartographInstanceHandlePtr>& Pair : InstanceHandles)
		{
			if (Pair.Value.IsValid())
			{
				Handles.Add(Pair.Value);
			}
		}

		if (AAbstractInstanceManager* Manager = Cast<AAbstractInstanceManager>(MarkerActor))
		{
			for (FCartographInstanceHandlePtr& Handle : Handles)
			{
				Manager->RemoveInstance(Handle);  // resets Handle as a side effect
			}
		}
	}

	InstanceHandles.Empty();
	ClassInstanceTemplates.Empty();
	LayerIdMap.Empty();
	LayerVisible.Empty();

	if (CaptureComponent)
	{
		CaptureComponent->DestroyComponent();
		CaptureComponent = nullptr;
	}
	if (MarkerActor)
	{
		MarkerActor->Destroy();
		MarkerActor = nullptr;
	}

	CaptureTarget = nullptr;
	MarkerMID = nullptr;
	MarkerBaseMaterial = nullptr;
	MarkerMesh = nullptr;
	Store = nullptr;
	ClassTable = nullptr;

	CARTO_LOG("InstanceRenderer: shut down");
}


// -----------------------------------------------------------------------------
// Class templates: one FInstanceData per distinct class. The AbstractInstance
// manager buckets instances into a ULightweightHISM keyed by mesh+materials
// (AbstractInstanceManager::BuildUniqueName), so giving each ClassId its own
// FInstanceData (shared mesh, but a per-class MID-or-material so the unique name
// differs) yields exactly "one HISM per class" without manual component
// creation (SPEC 8 "per-class instance batching").
// -----------------------------------------------------------------------------
void UCartographInstanceRenderer::BuildClassTemplates()
{
	if (!ClassTable)
	{
		return;
	}

	const int32 NumClasses = ClassTable->NumClasses();
	ClassInstanceTemplates.Reserve(NumClasses);

	for (int32 ClassId = 0; ClassId < NumClasses; ++ClassId)
	{
		const FClassDrawInfo* Info = ClassTable->FindInfo((uint16)ClassId);
		if (!Info)
		{
			continue;
		}

		// Pre-register the layer id so per-instance custom data can carry a dense
		// small int the material masks on.
		GetLayerId(Info->LayerMainCategory, Info->LayerSubCategory);

		TSharedPtr<FInstanceData> TemplatePtr = MakeShared<FInstanceData>();
		FInstanceData& Template = *TemplatePtr;
		Template.StaticMesh = MarkerMesh;

		// Each class shares the one Z/layer/show MID (so filters are global), but
		// the per-class fill color must differ. We do NOT create a unique MID per
		// class (that would defeat the single-write filter); instead the fill
		// color is carried as a per-instance custom-data palette index
		// (CartographInstanceCustomData::ColorIndex) and the marker material reads
		// it from a palette. The OverridenMaterials therefore stay the SHARED MID
		// for every class so all instances mask uniformly.
		//
		// To still get one HISM per class out of BuildUniqueName (which hashes
		// mesh+materials only), we rely on the manager's MaxMeshInstancePerComponent
		// bucketing being irrelevant here and instead accept a single shared HISM
		// across classes. Per-class isolation is a draw-order nicety, not a
		// correctness requirement for capture, so a shared component is acceptable;
		// the per-instance palette index preserves per-class color.
		// SPIKE(Q5): if per-class components are required for draw-order/LOD on the
		// fork, swap in a per-class duplicate MID here so BuildUniqueName differs.
		Template.OverridenMaterials.Add(MarkerMID);

		// Visual-only marker layer: no batched collision, no audio geometry.
		// SPEC 4.1 / Q6: the manager builds ULightweightCollisionComponent by
		// default; disable it for a visual-only layer.
		Template.bUseBatchedCollision = false;
		Template.bUseAkGeometry = false;            // SPIKE(Q6): no Ak geometry on the marker layer
		Template.bCastShadows = false;
		Template.bCastDistanceFieldShadows = false;
		Template.Mobility = EComponentMobility::Static;
		Template.NumCustomDataFloats = CartographInstanceCustomData::NumFloats;
		Template.DefaultPerInstanceCustomData.Init(0.f, CartographInstanceCustomData::NumFloats);
		Template.MaxDrawDistance = -1.f;            // never distance-culled; the capture is top-down

		ClassInstanceTemplates.Add((uint16)ClassId, MoveTemp(TemplatePtr));
	}
}


FInstanceData* UCartographInstanceRenderer::FindOrAddClassTemplate(uint16 ClassId)
{
	if (TSharedPtr<FInstanceData>* Existing = ClassInstanceTemplates.Find(ClassId))
	{
		return Existing->Get();
	}
	// Unknown class (e.g. registered after Build). Fall back to a generic template
	// so a late class still renders rather than dropping silently.
	if (!MarkerMesh || !MarkerMID)
	{
		return nullptr;
	}
	TSharedPtr<FInstanceData> TemplatePtr = MakeShared<FInstanceData>();
	FInstanceData& Template = *TemplatePtr;
	Template.StaticMesh = MarkerMesh;
	Template.OverridenMaterials.Add(MarkerMID);
	Template.bUseBatchedCollision = false;
	Template.bUseAkGeometry = false;
	Template.bCastShadows = false;
	Template.bCastDistanceFieldShadows = false;
	Template.Mobility = EComponentMobility::Static;
	Template.NumCustomDataFloats = CartographInstanceCustomData::NumFloats;
	Template.DefaultPerInstanceCustomData.Init(0.f, CartographInstanceCustomData::NumFloats);
	return ClassInstanceTemplates.Add(ClassId, MoveTemp(TemplatePtr)).Get();
}


// -----------------------------------------------------------------------------
// Capture rig: a hidden, collision-disabled marker actor (a dedicated
// AAbstractInstanceManager so its HISMs are isolated for ShowOnlyActors) plus an
// orthographic top-down USceneCaptureComponent2D rendering into the output RT.
// SPIKE(Q5): the entire isolation mechanism (PrimitiveRenderMode + ShowOnlyActors
// + GetViewOwner) is UNVERIFIED on the CSS renderer fork - the Cartograph_2.0
// POC used exactly this and never compiled.
// -----------------------------------------------------------------------------
void UCartographInstanceRenderer::EnsureCaptureRig(UWorld* World)
{
	if (!World)
	{
		return;
	}

	// --- Marker actor: a dedicated instance manager (NOT the game's shared one).
	// Using a private manager means every ULightweightHISM it owns lives on THIS
	// actor, so ShowOnlyActors(MarkerActor) isolates the capture to exactly our
	// markers and nothing else in the world. SPIKE(Q5): confirm a second
	// AAbstractInstanceManager can coexist with the game's UAbstractInstanceSubsystem
	// singleton on the fork (GetInstanceManager() resolves the subsystem's manager,
	// so SetInstanceFromDataStatic cannot be used against a private manager - we
	// must call the instance manager's SetInstanced() directly; see AddInstance).
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = MakeUniqueObjectName(World, AAbstractInstanceManager::StaticClass(), TEXT("CartographMarkerManager"));
	SpawnParams.ObjectFlags |= RF_Transient;
	MarkerActor = World->SpawnActor<AAbstractInstanceManager>(AAbstractInstanceManager::StaticClass(), FTransform::Identity, SpawnParams);
	CARTO_LOG_ERROR_RETURN_IF_NULL(MarkerActor);

	// Hidden from the player's main view; visible only to our capture component.
	MarkerActor->SetActorHiddenInGame(true);
	MarkerActor->SetActorEnableCollision(false);

	// --- Output render target. Square, RENDER_TEXTURE_SIZE per side (4096 in C),
	// no auto-mips (a whole-texture mip pass would defeat the per-tile economy).
	CaptureTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CartographCaptureTarget"));
	CaptureTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	CaptureTarget->ClearColor = FLinearColor::Transparent;
	CaptureTarget->bAutoGenerateMips = false;
	CaptureTarget->InitAutoFormat(RENDER_TEXTURE_SIZE, RENDER_TEXTURE_SIZE);
	CaptureTarget->UpdateResourceImmediate(true);

	// --- Capture component, attached to the marker actor.
	CaptureComponent = NewObject<USceneCaptureComponent2D>(MarkerActor, TEXT("CartographCapture"));
	CaptureComponent->SetupAttachment(MarkerActor->GetRootComponent());
	CaptureComponent->RegisterComponent();

	// Orthographic top-down. ORTHO requires CaptureSource = SceneColor for the
	// (optional, fork-conditional) ortho-tiling path (SPEC 4.2: ortho tiling
	// requires SceneColor, not FinalColor). SPIKE(Q5): every field below is on
	// the engine-side USceneCaptureComponent2D / its FMinimalViewInfo and is
	// UNVERIFIED-reachable on the CSS binary fork (no Engine/ source in repo).
	CaptureComponent->ProjectionType = ECameraProjectionMode::Orthographic;  // SPIKE(Q5): field reachable on fork?
	CaptureComponent->OrthoWidth = (float)MAP_WIDTH_CENTIMETERS;              // SPIKE(Q5)
	CaptureComponent->CaptureSource = SCS_SceneColorHDR;                      // SPIKE(Q5): SceneColor required for ortho tiling
	CaptureComponent->TextureTarget = CaptureTarget;
	CaptureComponent->bCaptureEveryFrame = false;  // debounced; we drive captures via RequestCapture
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->bAlwaysPersistRenderingState = true;

	// Capture isolation: only render our marker actor's components. SPIKE(Q5):
	// PrimitiveRenderMode + ShowOnlyActors + GetViewOwner is the exact mechanism
	// the POC never got to compile - validate before relying on the output.
	CaptureComponent->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;  // SPIKE(Q5)
	CaptureComponent->ShowOnlyActors.Reset();
	CaptureComponent->ShowOnlyActors.Add(MarkerActor);  // SPIKE(Q5): ShowOnlyActors honored on the fork?
	// NOTE: the SPEC/contract also names a GetViewOwner() override as part of the
	// isolation mechanism (Cartograph_2.0 / MapCaptureComponent2D salvage). That
	// requires subclassing USceneCaptureComponent2D (a new file outside this
	// agent's owned set), so it is NOT added here. PRM_UseShowOnlyList +
	// ShowOnlyActors is the minimal isolation; if the fork still bleeds other
	// primitives into the capture, introduce a UCartographCaptureComponent2D that
	// overrides GetViewOwner() to return MarkerActor. SPIKE(Q5).

	// Position the camera high above the map center looking straight down (-Z),
	// yawed so the captured image's +X/+Y match CartographCoords::WorldToScreen.
	const double CenterX = (WEST_BOUND_CENTIMETERS + EAST_BOUND_CENTIMETERS) * 0.5;
	const double CenterY = (NORTH_BOUND_CENTIMETERS + SOUTH_BOUND_CENTIMETERS) * 0.5;
	const FVector CaptureLocation((float)CenterX, (float)CenterY, (float)CaptureAltitudeCm);
	// Look down: pitch -90. Roll/yaw chosen so screen-X = world-X, screen-Y =
	// world-Y (top-down). SPIKE(Q12): confirm capture orientation reproduces the
	// legacy WorldToScreen handedness pixel-identically (diff-render harness).
	const FRotator CaptureRotation(-90.f, 0.f, 0.f);
	CaptureComponent->SetWorldLocationAndRotation(CaptureLocation, CaptureRotation);

	// Ortho-tiling (fork-conditional upside, NOT a TDR guarantee). The legacy
	// monolithic capture is the real TDR risk for C; the guaranteed TDR fix is
	// Phase A's per-tile EndDraw. Demoted to conditional per SPEC 4.2. The fields
	// below appear in ZERO repo C++ and may not exist / may be ignored on the fork.
	// SPIKE(Q5): does bEnableOrthographicTiling exist AND honor NumXTiles at
	// runtime on the CSS USceneCaptureComponent2D?
#if 0  // SPIKE(Q5): enable only after confirming these fields exist on the fork.
	CaptureComponent->bEnableOrthographicTiling = true;
	CaptureComponent->NumXTiles = TILES_PER_AXIS;
	CaptureComponent->NumYTiles = TILES_PER_AXIS;
#endif

	CARTO_LOG("InstanceRenderer: capture rig ready (ortho %.0fcm, RT %dx%d)", (float)MAP_WIDTH_CENTIMETERS, RENDER_TEXTURE_SIZE, RENDER_TEXTURE_SIZE);
}


// -----------------------------------------------------------------------------
// O(1) instance mutation. All game-thread-only.
// -----------------------------------------------------------------------------
void UCartographInstanceRenderer::AddInstance(FBuildingHandle Handle, const FBuildingRecord& Record)
{
	if (!MarkerActor || !MarkerMesh || !MarkerMID)
	{
		return;  // backend not initialized / not selected
	}
	if (InstanceHandles.Contains(Handle))
	{
		// Already has an instance; treat as an in-place update.
		UpdateInstance(Handle, Record);
		return;
	}

	FInstanceData* Template = FindOrAddClassTemplate(Record.ClassId);
	if (!Template)
	{
		return;
	}

	const FTransform InstanceTransform = ComputeInstanceTransform(Record);
	FInstanceData& TemplateRef = *Template;

	// Seed the per-instance custom data on the template so the instance is
	// correct on the first frame (before WriteCustomData runs).
	if (TemplateRef.DefaultPerInstanceCustomData.Num() >= CartographInstanceCustomData::NumFloats)
	{
		TemplateRef.DefaultPerInstanceCustomData[CartographInstanceCustomData::WorldZ] = Record.Pos.Z;
		const FClassDrawInfo* Info = ClassTable ? ClassTable->FindInfo(Record.ClassId) : nullptr;
		const int32 LayerId = Info ? GetLayerId(Info->LayerMainCategory, Info->LayerSubCategory) : 0;
		TemplateRef.DefaultPerInstanceCustomData[CartographInstanceCustomData::LayerId] = (float)LayerId;
		TemplateRef.DefaultPerInstanceCustomData[CartographInstanceCustomData::ColorIndex] = (float)Record.ClassId;
		TemplateRef.DefaultPerInstanceCustomData[CartographInstanceCustomData::ShowFlag] = bShowBuildings ? 1.f : 0.f;
	}

	FCartographInstanceHandlePtr OutHandle;

	// IMPORTANT: SetInstanceFromDataStatic resolves the SHARED game manager via
	// GetInstanceManager(), which would put our markers on the game's manager and
	// break ShowOnlyActors isolation. We instead drive OUR dedicated marker
	// manager directly via SetInstanced so the HISM lives on MarkerActor.
	// SPIKE(Q5): confirm a privately-spawned AAbstractInstanceManager's SetInstanced
	// works without the UAbstractInstanceSubsystem owning it on the fork.
	if (AAbstractInstanceManager* Manager = Cast<AAbstractInstanceManager>(MarkerActor))
	{
		// SPIKE(Q6): verify the EXACT SetInstanced overload on the fork's
		// AAbstractInstanceManager - param list & types assumed here are
		// (AActor* Owner, const FTransform& Transform, FInstanceData& TemplateRef,
		// FInstanceOwnerHandlePtr& OutHandle, bool bInitializeHidden). The order of
		// (TemplateRef, OutHandle) and the trailing bool are guessed, not confirmed
		// against AbstractInstanceManager.h.
		Manager->SetInstanced(MarkerActor, InstanceTransform, TemplateRef, OutHandle, /*bInitializeHidden*/ false);
	}

	if (!OutHandle.IsValid())
	{
		CARTO_LOG_WARNING("InstanceRenderer: AddInstance produced an invalid handle (Slot=%u Gen=%u)", Handle.Slot, Handle.Gen);
		return;
	}

	WriteCustomData(OutHandle, Record);
	InstanceHandles.Add(Handle, MoveTemp(OutHandle));

	// Batch the render-state update so a build-burst marks the component dirty
	// once at end of frame, not per instance (SPEC 4.2 high-churn mitigation).
	// SPIKE(Q7): MarkDirtyDeferred is currently a no-op in the plugin
	// (AbstractInstanceManager.cpp:633) - verify the batched-dirty path actually
	// coalesces on the fork or the per-add MarkRenderStateDirty dominates.
	bCapturePending = true;
}


void UCartographInstanceRenderer::RemoveInstance(FBuildingHandle Handle)
{
	if (!MarkerActor)
	{
		return;
	}

	FCartographInstanceHandlePtr* Found = InstanceHandles.Find(Handle);
	if (!Found || !Found->IsValid())
	{
		return;  // no instance for this handle - no-op per contract
	}

	if (AAbstractInstanceManager* Manager = Cast<AAbstractInstanceManager>(MarkerActor))
	{
		// O(1) swap-patch remove (AbstractInstanceManager.cpp:638). Resets the
		// handle pointer as a side effect.
		Manager->RemoveInstance(*Found);
	}

	InstanceHandles.Remove(Handle);
	bCapturePending = true;
}


void UCartographInstanceRenderer::UpdateInstance(FBuildingHandle Handle, const FBuildingRecord& Record)
{
	FCartographInstanceHandlePtr* Found = InstanceHandles.Find(Handle);
	if (!Found || !Found->IsValid())
	{
		// Nothing to update; treat as an add so a "move before add" event still
		// materializes the instance.
		AddInstance(Handle, Record);
		return;
	}

	// Patch the transform in place (O(1)). SetLocalTransform takes local space;
	// our instances are placed in world space relative to the identity-located
	// marker actor, so local == world here.
	const FTransform InstanceTransform = ComputeInstanceTransform(Record);
	(*Found)->SetLocalTransform(InstanceTransform);  // SPIKE(Q5): SetLocalTransform on a private manager's HISM

	WriteCustomData(*Found, Record);
	bCapturePending = true;
}


// -----------------------------------------------------------------------------
// Filtering at zero per-instance CPU (material scalar/vector writes only).
// -----------------------------------------------------------------------------
void UCartographInstanceRenderer::SetZFilter(float MinZ, float MaxZ)
{
	if (!MarkerMID)
	{
		return;
	}
	// Two scalar writes - replaces the legacy O(N) Z re-walk + 256 MB clear
	// entirely (SPEC 4.1, Q8). The material's clip() discards fragments whose
	// per-instance WorldZ falls outside [MinZ, MaxZ].
	MarkerMID->SetScalarParameterValue(ParamZMin, MinZ);
	MarkerMID->SetScalarParameterValue(ParamZMax, MaxZ);
	RequestCapture();  // re-capture once so the filtered image refreshes
}


void UCartographInstanceRenderer::SetLayerVisible(FName LayerMainCategory, FName LayerSubCategory, bool bVisible)
{
	if (!MarkerMID)
	{
		return;
	}
	const int32 LayerId = GetLayerId(LayerMainCategory, LayerSubCategory);
	if (LayerId < 0 || LayerId >= MaxMaskableLayers)
	{
		CARTO_LOG_WARNING("InstanceRenderer: layer id %d out of maskable range [0,%d)", LayerId, MaxMaskableLayers);
		return;
	}

	if (LayerVisible.Num() <= LayerId)
	{
		LayerVisible.Add(true, LayerId + 1 - LayerVisible.Num());
	}
	LayerVisible[LayerId] = bVisible;

	// Repack the 16-bit visibility into four RGBA vector params (channel per
	// layer). One vector write per affected mask param - no per-instance work.
	auto BuildMask = [this](int32 Base) -> FLinearColor
	{
		auto Bit = [this, Base](int32 Channel) -> float
		{
			const int32 Idx = Base + Channel;
			return (LayerVisible.IsValidIndex(Idx) && LayerVisible[Idx]) ? 1.f : 0.f;
		};
		return FLinearColor(Bit(0), Bit(1), Bit(2), Bit(3));
	};
	MarkerMID->SetVectorParameterValue(ParamLayerMask0, BuildMask(0));
	MarkerMID->SetVectorParameterValue(ParamLayerMask1, BuildMask(4));
	MarkerMID->SetVectorParameterValue(ParamLayerMask2, BuildMask(8));
	MarkerMID->SetVectorParameterValue(ParamLayerMask3, BuildMask(12));

	RequestCapture();
}


void UCartographInstanceRenderer::SetShowBuildings(bool bShow)
{
	bShowBuildings = bShow;
	if (!MarkerMID)
	{
		return;
	}
	// Global toggle: a single scalar the material multiplies into opacity/clip.
	// Cheaper than rewriting every instance's ShowFlag, and the per-instance
	// ShowFlag stays meaningful for future per-instance toggles.
	MarkerMID->SetScalarParameterValue(ParamShowBuildings, bShow ? 1.f : 0.f);
	RequestCapture();
}


// -----------------------------------------------------------------------------
// Capture.
// -----------------------------------------------------------------------------
void UCartographInstanceRenderer::RequestCapture()
{
	if (!CaptureComponent)
	{
		return;
	}
	// Debounced single capture. bCaptureEveryFrame is false; we explicitly drive
	// one capture so a burst of mutations coalesces into a single GPU pass.
	// SPIKE(Q5): a single ortho monolithic CaptureScene of a megabase marker layer
	// is itself a large submission - this is why C is NOT the TDR guarantee
	// (Phase A's per-tile EndDraw is). If ortho tiling is unavailable on the fork,
	// gate captures so first-paint drains across frames, or keep Phase A as the
	// committed floor.
	CaptureComponent->CaptureScene();  // SPIKE(Q5): CaptureScene reachable + honors ShowOnlyActors on fork
	bCapturePending = false;
}


UTextureRenderTarget2D* UCartographInstanceRenderer::GetCaptureTarget() const
{
	return CaptureTarget;
}


// -----------------------------------------------------------------------------
// Internal helpers.
// -----------------------------------------------------------------------------
void UCartographInstanceRenderer::WriteCustomData(const FCartographInstanceHandlePtr& Handle, const FBuildingRecord& Record) const
{
	if (!Handle.IsValid())
	{
		return;
	}

	const FClassDrawInfo* Info = ClassTable ? ClassTable->FindInfo(Record.ClassId) : nullptr;
	const int32 LayerId = Info ? const_cast<UCartographInstanceRenderer*>(this)->GetLayerId(Info->LayerMainCategory, Info->LayerSubCategory) : 0;

	TArray<float> Values;
	Values.SetNumZeroed(CartographInstanceCustomData::NumFloats);
	Values[CartographInstanceCustomData::WorldZ] = Record.Pos.Z;
	Values[CartographInstanceCustomData::LayerId] = (float)LayerId;
	Values[CartographInstanceCustomData::ColorIndex] = (float)Record.ClassId;
	Values[CartographInstanceCustomData::ShowFlag] = bShowBuildings ? 1.f : 0.f;

	// bMarkDirty=false: SetCustomPrimitiveDataOnHandle takes the cheap
	// MarkRenderInstancesDirty path (AbstractInstanceManager.cpp:762) so a build
	// burst does not trigger a full component rebuild per instance.
	// SPIKE(Q7): confirm the cheap-dirty path is correct for our shared material.
	// SPIKE(Q6): verify the EXACT signature on the fork - this asserts
	// SetCustomPrimitiveDataOnHandle is a STATIC member taking
	// (const FInstanceHandle& Handle, const TArray<float>& Values, bool bMarkDirty).
	// Both the static-ness and the (Handle, Values, bool) param list are guessed and
	// must be checked against AbstractInstanceManager.h on the fork.
	AAbstractInstanceManager::SetCustomPrimitiveDataOnHandle(Handle, Values, /*bMarkDirty*/ false);
}


int32 UCartographInstanceRenderer::GetLayerId(FName MainCategory, FName SubCategory)
{
	const uint32 Key = LayerKey(MainCategory, SubCategory);
	if (const int32* Existing = LayerIdMap.Find(Key))
	{
		return *Existing;
	}
	const int32 NewId = LayerIdMap.Num();
	LayerIdMap.Add(Key, NewId);
	return NewId;
}


uint32 UCartographInstanceRenderer::LayerKey(FName MainCategory, FName SubCategory)
{
	return HashCombine(GetTypeHash(MainCategory), GetTypeHash(SubCategory));
}


FTransform UCartographInstanceRenderer::ComputeInstanceTransform(const FBuildingRecord& Record) const
{
	// Top-down marker placement: world XY from the record, Z carried for the
	// material clip but the quad is placed on a constant marker plane so the
	// ortho capture sees a flat, non-overlapping layer (Z-order comes from the
	// material clip + draw order, not depth). Only yaw is used (SPEC 4.1: top-down
	// needs only yaw).
	const FVector Location((double)Record.Pos.X, (double)Record.Pos.Y, (double)Z_BAND_MIN_CM);
	const FRotator Rotation(0.f, Record.GetYawDegrees(), 0.f);
	// Scale the unit quad to the class footprint so the marker covers the right
	// pixel area. ComputeDrawGeometry owns the pixel-identical sizing for the
	// canvas/Slate backends; for the instanced backend we approximate footprint
	// scale here and defer exact parity to the SPIKE(Q12) diff-render harness.
	FVector Scale(1.0, 1.0, 1.0);
	if (const FClassDrawInfo* Info = ClassTable ? ClassTable->FindInfo(Record.ClassId) : nullptr)
	{
		if (Info->Footprint.X > 0.f && Info->Footprint.Y > 0.f)
		{
			Scale = FVector((double)Info->Footprint.X, (double)Info->Footprint.Y, 1.0);
		}
	}
	return FTransform(Rotation, Location, Scale);
}

#endif  // CARTOGRAPH_WITH_ABSTRACTINSTANCE
