#include "CartographGameInstanceModule.h"

#include <sstream>

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/CanvasPanelSlot.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Misc/OutputDeviceNull.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"

#include "FGLightweightBuildableSubsystem.h"
#include "Buildables/FGBuildable.h"
#include "FGBuildableBeam.h"
#include "FGBuildableSubsystem.h"
#include "Buildables/FGBuildableWire.h"
#include "Resources/FGBuildingDescriptor.h"
#include "FGBuildCategory.h"
#include "FGBuildSubCategory.h"
#include "FGPlayerController.h"
#include "FGSplineBuildableInterface.h"

#include "Configuration/Properties/ConfigPropertyString.h"
#include "ModLoading/ModLoadingLibrary.h"
#include "Patching/NativeHookManager.h"

#include "Util/CartographCanvasRenderItem.h"
#include "CartographModSubsystem.h"
#include "Cartograph_ConfigStruct.h"

// The new spine + render/net subsystems this module now owns.
#include "Core/CartographBuildingStore.h"
#include "Core/CartographSpatialGrid.h"
#include "Core/CartographZBandIndex.h"
#include "Core/CartographClassDrawTable.h"
#include "Core/CartographTileManager.h"
#include "Render/CartographCompositor.h"
#include "Net/CartographMapReplicator.h"


#define LOCTEXT_NAMESPACE "Cartograph"


DEFINE_LOG_CATEGORY(LogCartograph);


// =============================================================================
// Re-architected game-instance module (SPEC STAGE2).
//
// What changed vs the legacy file:
//  - DELETED: CurrentBuildingData (TArray<FBuildingData>), BuildingDataIndexRedirector,
//    CurrentBuildingQuadTree, OnBuildingDataAdd/Remove (the O(N) reindex triad),
//    RedrawMapCoroutine + RedrawMap + ExecuteRedrawMapCoroutine + OnCoroutineFinishedOrCancelled
//    (the cancel/restart, 256 MB single-clear, per-primitive-budget draw), and the
//    O(N^2)/by-value InitialBuildableGather.
//  - ADDED: the slim spine (store/grid/zband/classtable/tilemanager), the
//    never-cancelled compositor + Phase-0 ReliableMessaging replicator, O(1) build
//    hooks, and an O(N) const-ref streaming gather.
//  - The legacy FCanvas::GetBatchedElements scissor hook is REPOINTED at the
//    compositor's per-tile scissor (so each tile clip = a separate command buffer)
//    when the TiledCanvas backend is active.
// =============================================================================


// -----------------------------------------------------------------------------
// Yaw quantization helper. The slim record keeps only yaw (top-down). Extracted
// from the transform's rotator.
// -----------------------------------------------------------------------------
namespace
{
	uint16 PackTransformYaw(const FTransform& Transform)
	{
		return FBuildingRecord::PackYaw((float)Transform.GetRotation().Rotator().Yaw);
	}

	/** Resolve a class through the redirect map, matching the legacy FillInHash. */
	UClass* ResolveRedirect(UClass* OriginalClass)
	{
		if (!UCartographGameInstanceModule::Instance || !OriginalClass)
		{
			return OriginalClass;
		}
		// Explicit soft-class key: Find hashes/compares over the soft object path, not
		// via an implicit (and possibly explicit-marked) UClass* -> TSoftClassPtr ctor.
		const TSoftClassPtr<AFGBuildable>* Redirect =
			UCartographGameInstanceModule::Instance->BuildableClassRedirectMap.Find(TSoftClassPtr<AFGBuildable>(OriginalClass));
		if (Redirect)
		{
			if (UClass* Loaded = Redirect->LoadSynchronous())
			{
				return Loaded;
			}
		}
		return OriginalClass;
	}

	/**
	 * Compute a record's WORLD-space (cm, XY) bounding box directly from the slim
	 * record + class info + side-tables, WITHOUT round-tripping the screen-space
	 * VisualBox back through ScreenToWorld.
	 *
	 * The screen->world round-trip the call sites previously used inflates the box
	 * by the screen quantization (~183 cm/pixel at RENDER_TEXTURE_SIZE=4096), which
	 * makes a building span more grid cells than its true footprint and bloats grid
	 * occupancy / QueryRect false positives. This mirrors the SAME world-box math
	 * ComputeDrawGeometry accumulates internally (footprint corner union for
	 * Icon/Rectangle, endpoint union for Wire/Beam, point union for Spline),
	 * expanded by BOX_EXPANSION_CENTIMETERS in world cm exactly like the legacy
	 * FillInVisualBoxCache. Returns an invalid box for undrawable records (the
	 * caller then falls back to a point box at the anchor).
	 */
	FBox2f ComputeWorldVisualBox(const FBuildingRecord& Record, const FClassDrawInfo& Info, const FCartographBuildingStore& Store)
	{
		FBox2D WorldBox(ForceInit);

		const FVector2D WorldPosXY{ (double)Record.Pos.X, (double)Record.Pos.Y };

		switch (Info.DrawType)
		{
		case EBuildingDrawType::Spline:
		{
			if (Info.StrokeThickness <= 0.f || Record.ExtraIndex == (uint32)INDEX_NONE)
			{
				return FBox2f(ForceInit);
			}
			const FSplineExtra& Extra = Store.GetSplineExtra(Record.ExtraIndex);
			if (Extra.Points.Num() < 2)
			{
				return FBox2f(ForceInit);
			}
			for (const FVector2f& WorldPt : Extra.Points)
			{
				WorldBox += FVector2D{ (double)WorldPt.X, (double)WorldPt.Y };
			}
			break;
		}
		case EBuildingDrawType::Wire:
		{
			if (Info.StrokeThickness <= 0.f || Record.ExtraIndex == (uint32)INDEX_NONE)
			{
				return FBox2f(ForceInit);
			}
			const FWireExtra& Extra = Store.GetWireExtra(Record.ExtraIndex);
			WorldBox += WorldPosXY;
			WorldBox += FVector2D{ (double)Extra.End.X, (double)Extra.End.Y };
			break;
		}
		case EBuildingDrawType::Beam:
		{
			if (Info.StrokeThickness <= 0.f || Record.ExtraIndex == (uint32)INDEX_NONE)
			{
				return FBox2f(ForceInit);
			}
			const FBeamExtra& Extra = Store.GetBeamExtra(Record.ExtraIndex);
			const double YawRad = FMath::DegreesToRadians((double)Record.GetYawDegrees());
			const FVector2D ForwardXY{ FMath::Cos(YawRad), FMath::Sin(YawRad) };
			WorldBox += WorldPosXY;
			WorldBox += WorldPosXY + ForwardXY * (double)Extra.Length;
			break;
		}
		case EBuildingDrawType::Icon:
		case EBuildingDrawType::Rectangle:
		{
			if (Info.Footprint.X == 0.f || Info.Footprint.Y == 0.f)
			{
				return FBox2f(ForceInit);
			}
			// Yaw-only footprint corner union (scale = 1), matching the slim-record
			// corner transform in ComputeDrawGeometry.
			const double HalfWidth = (double)Info.Footprint.X / 2.0;
			const double HalfHeight = (double)Info.Footprint.Y / 2.0;
			const FVector LocalCorners[4] = {
				FVector(-HalfWidth, -HalfHeight, 0.0),
				FVector(HalfWidth, -HalfHeight, 0.0),
				FVector(HalfWidth, HalfHeight, 0.0),
				FVector(-HalfWidth, HalfHeight, 0.0)
			};
			const FRotator YawOnly(0.0, (double)Record.GetYawDegrees(), 0.0);
			const FTransform CornerTransform(YawOnly, FVector(WorldPosXY.X, WorldPosXY.Y, (double)Record.Pos.Z), FVector::OneVector);
			for (int32 i = 0; i < 4; ++i)
			{
				const FVector WorldCorner = CornerTransform.TransformPosition(LocalCorners[i]);
				WorldBox += FVector2D{ WorldCorner.X, WorldCorner.Y };
			}
			break;
		}
		case EBuildingDrawType::Invalid:
		default:
			return FBox2f(ForceInit);
		}

		if (!WorldBox.bIsValid)
		{
			return FBox2f(ForceInit);
		}
		// Expand in world cm exactly like legacy FillInVisualBoxCache.
		WorldBox = WorldBox.ExpandBy(BOX_EXPANSION_CENTIMETERS);
		return FBox2f(
			FVector2f((float)WorldBox.Min.X, (float)WorldBox.Min.Y),
			FVector2f((float)WorldBox.Max.X, (float)WorldBox.Max.Y));
	}
}


void UCartographGameInstanceModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);

	switch (Phase)
	{
	case ELifecyclePhase::CONSTRUCTION:
		return;

	case ELifecyclePhase::INITIALIZATION:
		RegisterMenuButton();
		return;

	case ELifecyclePhase::POST_INITIALIZATION:
		break;
	}

    CARTO_LOG("UCartographGameInstanceModule Init")

    GatherBuildables();
	GatherModOverrides();

	Instance = this;


	for (const auto& [Material, CategoryData] : MaterialBuildCategoryDataOverrideMap)
	{
		for (const auto& [_, Recipe] : GetMutableDefault<UFGFactoryCustomizationDescriptor_Material>(Material)->GetBuildableMap())
		{
			TSubclassOf<AFGBuildable> Buildable = GetDefault<UFGBuildingDescriptor>(UFGRecipe::GetDescriptorForRecipe(Recipe))->mBuildableClass;
			if (!BuildableBuildCategoryDataOverrideMap.Contains(Buildable.Get()))
			{
				BuildableBuildCategoryDataOverrideMap.Add(Buildable.Get(), CategoryData);
			}
		}
	}

	for (const auto& [Material, LayerData] : MaterialBuildLayerDataOverrideMap)
	{
		for (const auto& [_, Recipe] : GetMutableDefault<UFGFactoryCustomizationDescriptor_Material>(Material)->GetBuildableMap())
		{
			TSubclassOf<AFGBuildable> Buildable = GetDefault<UFGBuildingDescriptor>(UFGRecipe::GetDescriptorForRecipe(Recipe))->mBuildableClass;
			if (!BuildableBuildLayerDataOverrideMap.Contains(Buildable.Get()))
			{
				BuildableBuildLayerDataOverrideMap.Add(Buildable.Get(), LayerData);
			}
		}
	}

	LoadRuntimeConfig();

	// Build the per-class draw table ONCE here (O(distinct classes)), now that the
	// gather + overrides + layer caches are populated. Records resolve their ClassId
	// against this table; ComputeDrawGeometry reads its FClassDrawInfo at draw.
	FillBuildLayerDataCache();
	ClassDrawTable.Build(this);


#pragma region Hooking
	// -------------------------------------------------------------------------
	// Build hooks. POST-rearchitecture each hook does ONLY O(1) work: build a slim
	// record, insert it into the store + grid + Z-band index, mark the touched
	// tiles dirty, and (server) bump versions. No O(N) redirector, no coroutine
	// cancel, no canvas touch. The compositor converges the dirty tiles on tick.
	// -------------------------------------------------------------------------
    const auto LambdaAfterAddFromBuildableInstanceData =
        [this](int32 ReturnValue, AFGLightweightBuildableSubsystem* ClassInstance, TSubclassOf<AFGBuildable> BuildableClass,
            FRuntimeBuildableInstanceData& BuildableInstanceData, bool FromSaveData = false, int32 SaveDataBuildableIndex = INDEX_NONE,
            uint16 ConstructId = MAX_uint16, AActor* BuildEffectInstigator = nullptr, int32 BlueprintBuildEffectIndex = INDEX_NONE)
        {
			const bool ShouldSkip = ShouldInitialize || FromSaveData || IsClient || !GIsRunning;
			CARTO_LOG_VERBOSE("AddFromBuildableInstanceData: %s, Skip: %d", *BuildableClass->GetName(), ShouldSkip);
			if (ShouldSkip || BuildableToIgnore.Contains(BuildableClass.Get()))
			{
				return;
			}

			AddRecordFromTransform(BuildableClass, BuildableInstanceData.Transform, &BuildableInstanceData.TypeSpecificData, nullptr);
        };


	const auto LambdaAfterAddFromReplicatedData =
		[this](AFGLightweightBuildableSubsystem* ClassInstance, TSubclassOf<AFGBuildable> BuildableClass, TSubclassOf<UFGRecipe> BuiltWithRecipe,
			const FLightweightBuildableReplicationItem& ReplicationData, int32 MaxSize,
			AActor* BuildEffectInstigator, int32 BlueprintBuildIndex)
		{
			const bool ShouldSkip = ShouldInitialize || IsClient || !GIsRunning;
			CARTO_LOG_VERBOSE("AddFromReplicatedData: %s, Skip: %d", *BuildableClass->GetName(), ShouldSkip);
			if (ShouldSkip || BuildableToIgnore.Contains(BuildableClass.Get()))
			{
				return;
			}

			AddRecordFromTransform(BuildableClass, ReplicationData.Transform, &ReplicationData.TypeSpecificData, nullptr);
		};


	const auto LambdaAfterAddBuildable =
		[this](AFGBuildableSubsystem* ClassInstance, AFGBuildable* Buildable)
		{
            const bool ShouldSkip = ShouldInitialize || IsClient || !GIsRunning;
			CARTO_LOG_VERBOSE("AddBuildable: %s, Skip: %d", *Buildable->GetClass()->GetName(), ShouldSkip);
			if (ShouldSkip || BuildableToIgnore.Contains(Buildable->GetClass()))
			{
				return;
			}

			AddRecordFromTransform(Buildable->GetClass(), Buildable->GetTransform(), nullptr, Buildable);
		};


	const auto LambdaAfterInvalidateRuntimeInstanceDataForIndex =
		[this](AFGLightweightBuildableSubsystem* ClassInstance, TSubclassOf<AFGBuildable> BuildableClass, int32 Index)
		{
            const bool ShouldSkip = ShouldInitialize || IsClient || !GIsRunning;
			CARTO_LOG_VERBOSE("InvalidateRuntimeInstanceDataForIndex: %s, Skip: %d", *BuildableClass->GetName(), ShouldSkip);
			if (ShouldSkip || BuildableToIgnore.Contains(BuildableClass.Get()))
			{
				return;
			}

			const FRuntimeBuildableInstanceData* LightweightData = ClassInstance->GetRuntimeDataForBuildableClassAndIndex(BuildableClass, Index);
			CARTO_LOG_ERROR_RETURN_IF_NULL(LightweightData);

			const FBuildingHandle Handle = FindHandleForRemoval(BuildableClass, LightweightData->Transform);
			if (Handle.IsValid())
			{
				RemoveRecord(Handle);
			}
		};


	const auto LambdaAfterRemoveBuildable =
		[this](AFGBuildableSubsystem* ClassInstance, AFGBuildable* Buildable)
		{
			const bool ShouldSkip = ShouldInitialize || IsClient || !GIsRunning;
			CARTO_LOG_VERBOSE("RemoveBuildable: %s, Skip: %d", *Buildable->GetClass()->GetName(), ShouldSkip);
			if (ShouldSkip || BuildableToIgnore.Contains(Buildable->GetClass()))
			{
				return;
			}

			// For splines/wires the legacy stored a transform that came from the
			// component (not the actor); pass the live Buildable so FindHandleForRemoval
			// derives the same component position used at insert, so the probe matches.
			const FBuildingHandle Handle = FindHandleForRemoval(Buildable->GetClass(), Buildable->GetTransform(), Buildable);
			if (Handle.IsValid())
			{
				RemoveRecord(Handle);
			}
		};


	const auto LambdaAfterCloseRespawnUI =
        [this](AFGHUD* ClassInstance)
        {
            if (!ShouldInitialize || !GIsRunning)
            {
				return;
            }

            CARTO_LOG("CloseRespawnUI");

	        AFGPlayerController* PlayerController = Cast<AFGPlayerController>(GetWorld()->GetFirstPlayerController());
			CARTO_LOG_ERROR_RETURN_IF_NULL(PlayerController);
			if (PlayerController->HasAuthority())
			{
				return;
			}

			// Dedicated client: gather the buildings LOCALLY, the same budgeted
			// coroutine the host runs. The original re-arch tried a "server computes,
			// client pulls AoI tiles" path here (ReplComp->RequestAoI), but the
			// ReliableMessaging transport was never actually wired (SPIKE Q1): the
			// component is looked up passively and the handshake is never driven, so
			// RequestAoI silently no-op'd, the client received no tiles, and the map
			// sat empty at "Initializing..(0%)" forever (InitializeProgress is only
			// ever written by StreamingGather, which used to be host-only).
			// Buildables + lightweight instances ARE replicated to clients, so the
			// client can enumerate them itself; StreamingGather populates the store,
			// advances InitializeProgress, clears IsInitializing, and first-paints via
			// SetFullRedraw. The compositor DEBOUNCES that paint on the TileManager dirty
			// epoch (r.Cartograph.DrainDebounceTicks), so it converges once AFTER the
			// ~18k-building stream settles - O(N), not the O(N^2) per-streamed-building
			// re-render that OOM-killed the client. NM_Client only reaches here.
			ShouldInitialize = false;
			IsInitializing = true;
			InitializeSpine(GetWorld());
			GatherCoroutine = StreamingGather();
        };


	if (!WITH_EDITOR)
	{
		// Doing it after PlayerController::BeginPlay would interfere other network packets,
	    // resulting higher chance of packet loss (due to timeout)
		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGHUD, CloseRespawnUI, LambdaAfterCloseRespawnUI);


		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGLightweightBuildableSubsystem, AddFromBuildableInstanceData, LambdaAfterAddFromBuildableInstanceData);
	    SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGLightweightBuildableSubsystem, AddFromReplicatedData, LambdaAfterAddFromReplicatedData);

		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGBuildableSubsystem, AddBuildable, LambdaAfterAddBuildable);


		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGLightweightBuildableSubsystem, InvalidateRuntimeInstanceDataForIndex, LambdaAfterInvalidateRuntimeInstanceDataForIndex);

		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGBuildableSubsystem, RemoveBuildable, LambdaAfterRemoveBuildable);


		// The global FCanvas::GetBatchedElements scissor hook. POST-rearchitecture it
		// reads the COMPOSITOR's active per-tile scissor (Compositor.GetScissorForCanvas)
		// instead of the deleted UCartographGameInstanceModule::ScissorArea, so each
		// tile's clip is a separate command buffer (the TDR fix, SPEC 4.2). It is only
		// meaningful for the TiledCanvas backend; the Slate/instance backends never
		// open this FCanvas, so GetScissorForCanvas returns false and the hook is inert.
		// Alias std::array<uint32, 4> here: a bare comma inside the lambda below would
		// split the SUBSCRIBE_METHOD function-like-macro argument and fail to compile.
		using FScissorRect = std::array<uint32, 4>;
		SUBSCRIBE_METHOD(FCanvas::GetBatchedElements,
			[](auto& Scope, FCanvas* ClassInstance,
				FCanvas::EElementType InElementType, FBatchedElementParameters* InBatchedElementParameters, const FTexture* InTexture, ESimpleElementBlendMode InBlendMode, const FDepthFieldGlowInfo& GlowInfo, bool bApplyDPIScale)
			{
				// Only intercept the compositor's own canvas while it is mid-tile.
				if (!Instance)
				{
					return;
				}
				FScissorRect _ScissorUnused;
				if (!Instance->Compositor.GetScissorForCanvas(ClassInstance, _ScissorUnused))
				{
					return;
				}

				// get sort element based on the current sort key from top of sort key stack
				FCanvas::FCanvasSortElement& SortElement = ClassInstance->GetSortElement(ClassInstance->TopDepthSortKey());
				// find a batch to use
				FCartographCanvasRenderItem* RenderBatch = nullptr;
				// get the current transform entry from top of transform stack
				FCanvas::FTransformEntry FinalTransform = ClassInstance->GetTransformStack().Top();

				if (!bApplyDPIScale && ClassInstance->GetDPIScale() != 1.0f)
				{
					FinalTransform = FCanvas::FTransformEntry(FScaleMatrix(1 / ClassInstance->GetDPIScale()) * FinalTransform.GetMatrix());
				}

				// try to use the current top entry in the render batch array
				if (SortElement.RenderBatchArray.Num() > 0)
				{
					checkSlow(SortElement.RenderBatchArray.Last());
					RenderBatch = static_cast<FCartographCanvasRenderItem*>(SortElement.RenderBatchArray.Last());
				}

				// if a matching entry for this batch doesn't exist then allocate a new entry
				if (RenderBatch == nullptr ||
					!RenderBatch->IsMatch(InBatchedElementParameters, InTexture, InBlendMode, InElementType, FinalTransform, GlowInfo))
				{
					RenderBatch = new FCartographCanvasRenderItem(InBatchedElementParameters, InTexture, InBlendMode, InElementType, FinalTransform, GlowInfo);
					SortElement.RenderBatchArray.Add(RenderBatch);
				}

				Scope.Override(RenderBatch->GetBatchedElements());
			});
	}
#pragma endregion
}


// =============================================================================
// Spine lifecycle.
// =============================================================================
void UCartographGameInstanceModule::InitializeSpine(UWorld* World)
{
	if (bSpineInitialized)
	{
		return;
	}
	if (!World)
	{
		CARTO_LOG_ERROR("InitializeSpine with null world");
		return;
	}

	// Allocate the index structures (flat arrays sized off the constexpr geometry).
	SpatialGrid.Initialize();
	ZBandIndex.Initialize();
	TileManager.Initialize();

	// The class draw table was built in DispatchLifecycleEvent (POST_INITIALIZATION);
	// rebuild defensively if it is empty (e.g. a late world-load before init ran).
	if (ClassDrawTable.NumClasses() == 0)
	{
		FillBuildLayerDataCache();
		ClassDrawTable.Build(this);
	}

	// Dense ClassId -> class-hash reverse, so RemoveRecord decrements BuildingCountMap
	// in O(1). The table assigns ClassIds keyed by the ORIGINAL (pre-redirect) class
	// (see CartographClassDrawTable::Build), which is exactly the key in
	// ClassPtrToClassIDMap, so the hash is recoverable directly.
	ClassIdToHash.Init(0, ClassDrawTable.NumClasses());
	for (const auto& [Class, Hash] : ClassPtrToClassIDMap)
	{
		const uint16 Id = ClassDrawTable.GetClassId(Class.Get());
		if (Id != FCartographClassDrawTable::InvalidClassId && ClassIdToHash.IsValidIndex(Id))
		{
			ClassIdToHash[Id] = Hash;
		}
	}

	bIsDedicatedServer = FPlatformProperties::IsServerOnly();

	if (bIsDedicatedServer)
	{
		// Dedicated server renders NOTHING: no atlas, no compositor (SPEC 4.5). It
		// only maintains indices + per-tile versions + slim blobs. Bind the
		// replicator so joining clients can pull AoI tiles.
		MapReplicator.Initialize(&BuildingStore, &SpatialGrid, &TileManager);
		CARTO_LOG("Spine initialized (dedicated server: indices + replicator only)");
	}
	else
	{
		// Host (listen / SP) and dedicated client render locally through the
		// compositor onto the persistent atlas. The atlas is a UPROPERTY already
		// assigned from the asset (RenderTarget); enforce the mip invariant + bind
		// the compositor + start the never-cancelled drain ONCE.
		if (RenderTarget)
		{
			RenderTarget->bAutoGenerateMips = false;

			// Atlas-size invariant (SPEC 4.2). The compositor draws in ABSOLUTE
			// RENDER_TEXTURE_SIZE pixel space (CartographTypes::WorldToScreen returns
			// uv * RENDER_TEXTURE_SIZE), but the bundled CanvasRenderTarget_BuildingVisualization
			// .uasset was authored at 8192 and never re-saved after RENDER_TEXTURE_SIZE was
			// dropped 8192 -> 4096. On an oversized resource every draw lands in the top-left
			// RENDER_TEXTURE_SIZE-square quadrant and the map UI (which samples the whole RT)
			// shows the base squished into the upper-left corner. Resize at runtime so the atlas
			// always matches the draw space regardless of the asset's saved size (also restores
			// the intended 256 MB -> 64 MB cut). Must run BEFORE Compositor.Initialize, which
			// captures + pins the resource. Mirrors the InstanceRenderer init pattern.
			if (RenderTarget->SizeX != RENDER_TEXTURE_SIZE || RenderTarget->SizeY != RENDER_TEXTURE_SIZE)
			{
				CARTO_LOG_ERROR("Atlas RT is %dx%d, expected %dx%d - resizing at runtime (asset is stale; re-save CanvasRenderTarget_BuildingVisualization at %d)",
					RenderTarget->SizeX, RenderTarget->SizeY, RENDER_TEXTURE_SIZE, RENDER_TEXTURE_SIZE, RENDER_TEXTURE_SIZE);
				RenderTarget->InitAutoFormat(RENDER_TEXTURE_SIZE, RENDER_TEXTURE_SIZE);
				RenderTarget->UpdateResourceImmediate(true);
			}

			Compositor.Initialize(&BuildingStore, &SpatialGrid, &ZBandIndex, &ClassDrawTable, &TileManager, RenderTarget);

			// Start the never-cancelled convergent loop ONCE. It runs until ShutdownSpine
			// flips the compositor's bRunning false; it is never Cancel()'d (SPEC 4.3).
			// Pass `this` (a UObject) as the latent context: the compositor is a plain
			// C++ object and cannot be its own UE5Coro world context, so the owning
			// module supplies the latent-action Target + world explicitly.
			CompositorCoroutine = Compositor.TickConverge(this);
		}
		else
		{
			CARTO_LOG_ERROR("RenderTarget is null on a rendering build; the map will not draw");
		}

		// A listen host also runs the replicator so dedicated clients can join it.
		if (!IsClient)
		{
			MapReplicator.Initialize(&BuildingStore, &SpatialGrid, &TileManager);
		}

		CARTO_LOG("Spine initialized (rendering build; backend %d)", (int32)CartographConfig::GetRenderBackend());
	}

	bSpineInitialized = true;
}


void UCartographGameInstanceModule::ShutdownSpine()
{
	if (!bSpineInitialized)
	{
		return;
	}

	// Stop the never-cancelled compositor cleanly (Shutdown flips bRunning so the
	// loop co_returns; we do NOT Cancel()).
	Compositor.Shutdown();
	CompositorCoroutine = UE5Coro::TCoroutine<>::CompletedCoroutine;

	if (!GatherCoroutine.IsDone())
	{
		GatherCoroutine.Cancel();
	}
	GatherCoroutine = UE5Coro::TCoroutine<>::CompletedCoroutine;

	BuildingStore.Empty();
	SpatialGrid.Reset();
	ZBandIndex.Reset();
	TileManager.Initialize();  // clears dirty + versions
	BuildingCountMap.Empty();

	bSpineInitialized = false;
}


void UCartographGameInstanceModule::OnWorldLoaded(UWorld* World)
{
	CARTO_LOG("OnWorldLoaded");

    IsInWorld = true;
	IsClient = GetWorld()->IsNetMode(NM_Client);

	// Build the spine up-front (host/server) so the build hooks have somewhere to
	// write. On a dedicated client the spine is initialized at CloseRespawnUI when
	// the AoI pull begins, but initialize it here too so version pings/early tiles
	// have a target; InitializeSpine is idempotent.
	InitializeSpine(World);

	if (!IsClient)
	{
		// Wait for ACartographModSubsystem to initialize, then run the O(N) const-ref
		// streaming bucketing gather (replaces the O(N^2) by-value InitialBuildableGather).
		GetWorld()->GetTimerManager().SetTimerForNextTick(
			[this]()
			{
				if (!ShouldInitialize || !GIsRunning)
				{
					return;
				}

				ShouldInitialize = false;
				IsInitializing = true;
				GatherCoroutine = StreamingGather();
			});
	}

	if (!FPlatformProperties::IsServerOnly() && RenderTarget)
	{
		// Clear the atlas ONCE on load. After this the compositor only ever does
		// per-tile scissored clears (never a full 256 MB opaque clear again).
		UKismetRenderingLibrary::ClearRenderTarget2D(this, RenderTarget, { 0, 0, 0, 0 });
	}
}


void UCartographGameInstanceModule::OnWorldUnloaded()
{
	CARTO_LOG("OnWorldUnloaded");

	IsInWorld = false;
	ShutdownSpine();
}


void UCartographGameInstanceModule::OnLayerConfigChanged()
{
	CARTO_LOG_DEBUG("OnLayerConfigChanged");

	// A layer toggle changes WHICH buildings draw. The compositor re-converges by
	// marking every populated tile dirty (a bounded per-tile re-draw, NOT the legacy
	// O(N) re-walk + 256 MB clear). SetFullRedraw is exactly the #10 full-redraw path.
	if (!bIsDedicatedServer)
	{
		Compositor.SetFullRedraw();
	}
	SaveRuntimeConfig();
}


const FBuildLayerData* UCartographGameInstanceModule::GetBuildLayerData(uint32 ClassHash)
{
	FillBuildLayerDataCache();

    if (const auto* DataCache = BuildLayerDataMapCache.Find(ClassHash))
    {
        return *DataCache;
    }
	return nullptr;
}


bool UCartographGameInstanceModule::DoesBuildingExist(uint32 ClassHash) const
{
	return BuildingCountMap.FindRef(ClassHash) > 0;
}


// =============================================================================
// O(1) build-hook helpers (the hot path).
// =============================================================================

FBuildingHandle UCartographGameInstanceModule::AddRecordFromTransform(
	const TSubclassOf<AFGBuildable>& BuildableClass, const FTransform& Transform,
	const FFGDynamicStruct* TypeSpecificData, AFGBuildable* LiveBuildable)
{
	if (!bSpineInitialized)
	{
		return FBuildingHandle{};
	}

	UClass* OriginalClass = BuildableClass.Get();
	if (!OriginalClass)
	{
		return FBuildingHandle{};
	}

	// Resolve the dense ClassId (built once by ClassDrawTable). Records always get a
	// ClassId so they resolve at draw; an Invalid/undrawable class still indexes but
	// ComputeDrawGeometry early-outs on it (mirrors legacy "no cache" classes).
	const uint16 ClassId = ClassDrawTable.GetClassId(OriginalClass);
	if (ClassId == FCartographClassDrawTable::InvalidClassId)
	{
		CARTO_LOG_VERBOSE("No ClassId for %s; not tracked", *OriginalClass->GetName());
		return FBuildingHandle{};
	}

	const FClassDrawInfo& Info = ClassDrawTable.GetInfo(ClassId);

	// Build the slim record. For splines/wires the legacy derived Pos from the
	// component (spline component transform / wire connection 0), not the actor; we
	// reproduce that so Pos == the legacy stored position (and the side-table
	// extras match), keeping the rendered map pixel-identical (SPEC Q12).
	FBuildingRecord Record;
	Record.ClassId = ClassId;
	Record.Type = Info.DrawType;
	Record.ExtraIndex = (uint32)INDEX_NONE;

	FTransform EffectiveTransform = Transform;

	switch (Info.DrawType)
	{
	case EBuildingDrawType::Spline:
	{
		// Sample the spline polyline. Only available from a live buildable (the
		// component); the lightweight/replicated paths do not carry a spline, so a
		// spline that arrives without a live actor is dropped (legacy did the same -
		// it pulled GetSplineComponent from the cast buildable).
		const IFGSplineBuildableInterface* Spline = Cast<IFGSplineBuildableInterface>(LiveBuildable);
		const USplineComponent* SplineComponent = Spline ? Spline->GetSplineComponent() : nullptr;
		if (!SplineComponent)
		{
			return FBuildingHandle{};
		}

		EffectiveTransform = SplineComponent->GetComponentTransform();

		FSplineExtra Extra;
		Extra.Points.Reserve(SPLINE_SEGMENTS + 1);
		const float Step = SplineComponent->Duration / SPLINE_SEGMENTS;
		for (int32 i = 0; i < SPLINE_SEGMENTS + 1; ++i)
		{
			const FVector P = SplineComponent->GetLocationAtTime(i * Step, ESplineCoordinateSpace::World, true);
			Extra.Points.Add(FVector2f((float)P.X, (float)P.Y));
		}
		Record.ExtraIndex = BuildingStore.AddSplineExtra(Extra);
		break;
	}

	case EBuildingDrawType::Wire:
	{
		const AFGBuildableWire* Wire = Cast<AFGBuildableWire>(LiveBuildable);
		if (!Wire)
		{
			return FBuildingHandle{};
		}
		// Legacy: location = connection 0, end = connection 1.
		const FVector Start = Wire->GetConnectionLocation(0);
		const FVector End = Wire->GetConnectionLocation(1);
		EffectiveTransform.SetLocation(Start);

		FWireExtra Extra;
		Extra.End = FVector2f((float)End.X, (float)End.Y);
		Record.ExtraIndex = BuildingStore.AddWireExtra(Extra);
		break;
	}

	case EBuildingDrawType::Beam:
	{
		// Beam length comes from the lightweight type-specific data.
		float Length = 0.f;
		if (TypeSpecificData)
		{
			if (const auto* BeamData = TypeSpecificData->GetValuePtr<FBuildableBeamLightweightData>())
			{
				Length = BeamData->BeamLength;
			}
		}
		FBeamExtra Extra;
		Extra.Length = Length;
		Record.ExtraIndex = BuildingStore.AddBeamExtra(Extra);
		break;
	}

	default:
		break;
	}

	const FVector Location = EffectiveTransform.GetLocation();
	Record.Pos = FVector3f((float)Location.X, (float)Location.Y, (float)Location.Z);
	Record.PackedYaw = PackTransformYaw(EffectiveTransform);

	const FBuildingHandle Handle = InsertRecord(Record);

	// Live count (UI DoesBuildingExist). Keyed by class hash, recovered O(1) from
	// the dense ClassId via ClassIdToHash.
	if (ClassIdToHash.IsValidIndex(ClassId))
	{
		BuildingCountMap.FindOrAdd(ClassIdToHash[ClassId])++;
	}

	return Handle;
}


FBuildingHandle UCartographGameInstanceModule::InsertRecord(const FBuildingRecord& Record)
{
	const FBuildingHandle Handle = BuildingStore.Add(Record);

	// Compute the screen-space draw geometry ONCE here so we know exactly which
	// tiles/cells this building covers. The VisualBox (screen px, world-expanded) is
	// the dirty-rect key; the grid is world-keyed so we hand it a world box.
	const FClassDrawInfo* Info = ClassDrawTable.FindInfo(Record.ClassId);
	FDrawGeometry Geometry;
	const bool bDrawable = Info && FCartographClassDrawTable::ComputeDrawGeometry(Record, *Info, BuildingStore, Geometry);

	// World box for the grid: compute the world-space VisualBox DIRECTLY from the
	// record + class footprint (no screen->world round-trip, which would inflate the
	// box by the screen quantization and bloat grid occupancy). Fall back to a point
	// box at the anchor for undrawable records so the grid still tracks them for
	// picking / removal probing.
	FBox2f WorldBox(ForceInit);
	if (bDrawable && Info)
	{
		WorldBox = ComputeWorldVisualBox(Record, *Info, BuildingStore);
	}
	if (!WorldBox.bIsValid)
	{
		const FVector2f Anchor(Record.Pos.X, Record.Pos.Y);
		WorldBox = FBox2f(Anchor, Anchor);
	}

	SpatialGrid.Insert(Handle, WorldBox);

	// The Z-band index is only CONSUMED by the SlateInstanced backend (which uses
	// QueryBandRange for painters'-order instance emission). The default TiledCanvas
	// compositor filters by an exact per-record Z test in DrawTileIntoCanvas and never
	// queries the bands, so maintaining the index on the default path is pure O(1)
	// dead weight per insert/erase. Gate the maintenance on the active backend so the
	// default path does not pay for an unused index (matched in EraseRecord).
	if (CartographConfig::GetRenderBackend() == ECartographRenderBackend::SlateInstanced)
	{
		ZBandIndex.Insert(Handle, Record.Pos.Z);
	}

	// Mark every tile the screen VisualBox touches dirty (O(tiles covered)), and on
	// any authority bump those tiles' versions so AoI clients pull the delta.
	if (bDrawable && Geometry.VisualBox.bIsValid)
	{
		TileManager.MarkDirtyForBox(Geometry.VisualBox);

		if (!IsClient)
		{
			TArray<FTileId> Tiles;
			FCartographTileManager::GetTilesForBox(Geometry.VisualBox, Tiles);
			for (const FTileId Tile : Tiles)
			{
				TileManager.BumpVersion(Tile);
			}
		}
	}

	return Handle;
}


void UCartographGameInstanceModule::EraseRecord(FBuildingHandle Handle)
{
	const FBuildingRecord* Record = BuildingStore.Find(Handle);
	if (!Record)
	{
		return;
	}

	// Recompute the same geometry used at insert so we remove from EXACTLY the cells
	// it was inserted into (grid contract: same box to Remove as Insert) and dirty
	// the same tiles. ComputeDrawGeometry is pure, so this is deterministic.
	const FClassDrawInfo* Info = ClassDrawTable.FindInfo(Record->ClassId);
	FDrawGeometry Geometry;
	const bool bDrawable = Info && FCartographClassDrawTable::ComputeDrawGeometry(*Record, *Info, BuildingStore, Geometry);

	// Derive the world box with the SAME direct computation Insert used so Remove
	// targets EXACTLY the cells the record was inserted into (grid contract: same box
	// to Remove as Insert). ComputeWorldVisualBox is pure, so this is deterministic.
	FBox2f WorldBox(ForceInit);
	if (bDrawable && Info)
	{
		WorldBox = ComputeWorldVisualBox(*Record, *Info, BuildingStore);
	}
	if (!WorldBox.bIsValid)
	{
		const FVector2f Anchor(Record->Pos.X, Record->Pos.Y);
		WorldBox = FBox2f(Anchor, Anchor);
	}

	SpatialGrid.Remove(Handle, WorldBox);

	// Mirror the InsertRecord gating: only the SlateInstanced backend consumes the
	// Z-band index, so we only maintain it on that path. Removing unconditionally
	// here while Insert was gated would underflow a band; the matched condition keeps
	// Insert/Remove balanced regardless of the backend.
	if (CartographConfig::GetRenderBackend() == ECartographRenderBackend::SlateInstanced)
	{
		ZBandIndex.Remove(Handle, Record->Pos.Z);
	}

	if (bDrawable && Geometry.VisualBox.bIsValid)
	{
		TileManager.MarkDirtyForBox(Geometry.VisualBox);

		if (!IsClient)
		{
			TArray<FTileId> Tiles;
			FCartographTileManager::GetTilesForBox(Geometry.VisualBox, Tiles);
			for (const FTileId Tile : Tiles)
			{
				TileManager.BumpVersion(Tile);
			}
		}
	}

	// Free the store slot. Remove() ALSO frees the matching typed side-table entry
	// per its documented contract (CartographBuildingStore.cpp Remove: it switches on
	// Record->Type and calls RemoveSplineExtra/RemoveWireExtra/RemoveBeamExtra), then
	// bumps the slot generation so the freed slot rejects any stale handle (ABA-safety).
	// We must NOT free the side-table entry manually here too: doing so would push the
	// same ExtraIndex onto the per-table free-list twice, so the next two AddExtra calls
	// would alias one slot and corrupt spline/wire/beam geometry on any churned save.
	BuildingStore.Remove(Handle);
}


void UCartographGameInstanceModule::RemoveRecord(FBuildingHandle Handle)
{
	const FBuildingRecord* Record = BuildingStore.Find(Handle);
	if (!Record)
	{
		return;
	}

	// Decrement the live count for the UI (DoesBuildingExist) in O(1) via the dense
	// ClassId -> hash reverse, before the record is freed.
	if (ClassIdToHash.IsValidIndex(Record->ClassId))
	{
		if (uint32* Count = BuildingCountMap.Find(ClassIdToHash[Record->ClassId]))
		{
			if (*Count > 0) { --(*Count); }
		}
	}

	EraseRecord(Handle);
}


FBuildingHandle UCartographGameInstanceModule::FindHandleForRemoval(
	const TSubclassOf<AFGBuildable>& BuildableClass, const FTransform& Transform, AFGBuildable* LiveBuildable) const
{
	if (!bSpineInitialized)
	{
		return FBuildingHandle{};
	}

	UClass* OriginalClass = BuildableClass.Get();
	if (!OriginalClass)
	{
		return FBuildingHandle{};
	}
	const uint16 ClassId = ClassDrawTable.GetClassId(OriginalClass);
	if (ClassId == FCartographClassDrawTable::InvalidClassId)
	{
		return FBuildingHandle{};
	}

	// Derive the SAME effective position used at insert so the probe matches the
	// stored Pos: splines use the spline-component transform, wires use connection 0.
	FVector Location = Transform.GetLocation();
	const FClassDrawInfo* Info = ClassDrawTable.FindInfo(ClassId);
	if (Info && LiveBuildable)
	{
		if (Info->DrawType == EBuildingDrawType::Spline)
		{
			if (const IFGSplineBuildableInterface* Spline = Cast<IFGSplineBuildableInterface>(LiveBuildable))
			{
				if (const USplineComponent* SplineComponent = Spline->GetSplineComponent())
				{
					Location = SplineComponent->GetComponentTransform().GetLocation();
				}
			}
		}
		else if (Info->DrawType == EBuildingDrawType::Wire)
		{
			if (const AFGBuildableWire* Wire = Cast<AFGBuildableWire>(LiveBuildable))
			{
				Location = Wire->GetConnectionLocation(0);
			}
		}
	}

	// Probe the grid at the removed building's anchor and match by (ClassId, position).
	// Identity is the handle now (SPEC Q13), but the engine remove event gives us only
	// a class + transform, so we find the live handle by a generation-guarded position
	// probe rather than an exact-equality compare on the (now single-precision) Pos.
	// SPIKE(Q11): the SML remove hooks (RemoveBuildable / InvalidateRuntimeInstanceDataForIndex)
	// give a class + transform, NOT our handle, so we resolve the handle by position.
	// For splines/wires the legacy stored a component-derived position that this probe
	// uses the actor transform for, which may miss; validate removal fidelity against
	// a real engine-side mass removal (mass-dismantle) on the fork. A miss leaks a
	// stale record until the next reconcile/full-rebuild, never a crash.
	const FVector2f Anchor((float)Location.X, (float)Location.Y);
	const FBox2f ProbeBox(Anchor, Anchor);

	FBuildingHandle Best{};
	float BestDistSq = TNumericLimits<float>::Max();
	const FVector3f Target((float)Location.X, (float)Location.Y, (float)Location.Z);

	SpatialGrid.QueryRect(ProbeBox, [&](FBuildingHandle Handle)
	{
		const FBuildingRecord* Record = BuildingStore.Find(Handle);
		if (!Record || Record->ClassId != ClassId)
		{
			return;
		}
		const float DistSq = (Record->Pos - Target).SizeSquared();
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Handle;
		}
	});

	// Accept only a near-exact match (within 1 cm^2) so we never remove the wrong
	// building when two of the same class are in the same grid cell.
	constexpr float MaxMatchDistSq = 1.0f;
	if (Best.IsValid() && BestDistSq <= MaxMatchDistSq)
	{
		return Best;
	}
	return FBuildingHandle{};
}


// =============================================================================
// O(N) const-ref streaming gather (replaces the O(N^2)/by-value gather).
// =============================================================================
UE5Coro::TCoroutine<> UCartographGameInstanceModule::StreamingGather(FForceLatentCoroutine)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		IsInitializing = false;
		co_return;
	}

	AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(World);
	AFGLightweightBuildableSubsystem* LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(World);

	// Const-ref iteration over the engine arrays - NO by-value engine-map deep copy
	// (deletes initial-load-full-copy-by-value). The engine is the source of truth.
	const TArray<AFGBuildable*>& Buildables = BuildableSubsystem->GetAllBuildablesRef();
	const TMap<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Lightweight =
		LightweightSubsystem->GetAllLightweightBuildableInstances();

	int32 LightweightCount = 0;
	for (const auto& [Type, Arr] : Lightweight)
	{
		LightweightCount += Arr.Num();
	}
	const int32 Total = FMath::Max(1, Buildables.Num() + LightweightCount);
	CARTO_LOG("StreamingGather Started. Buildables: %d, Lightweight: %d", Buildables.Num(), LightweightCount);

	BuildingStore.Reserve(Total);

	const float TimeBudget = FCartograph_ConfigStruct::GetActiveConfig(World).InitializeTimeBudget;
	UE5Coro::Latent::FTickTimeBudget Budget = UE5Coro::Latent::FTickTimeBudget::Milliseconds(TimeBudget);

	int32 Processed = 0;

	for (AFGBuildable* Buildable : Buildables)
	{
		if (IsValid(Buildable) && !BuildableToIgnore.Contains(Buildable->GetClass()))
		{
			AddRecordFromTransform(Buildable->GetClass(), Buildable->GetTransform(), nullptr, Buildable);
		}
		InitializeProgress = (float)(++Processed) / Total;
		co_await Budget;
	}

	for (const auto& [Type, Arr] : Lightweight)
	{
		if (BuildableToIgnore.Contains(Type.Get()))
		{
			Processed += Arr.Num();
			continue;
		}
		for (const FRuntimeBuildableInstanceData& InstanceData : Arr)
		{
			AddRecordFromTransform(Type, InstanceData.Transform, &InstanceData.TypeSpecificData, nullptr);
			InitializeProgress = (float)(++Processed) / Total;
			co_await Budget;
		}
	}

	// Height slider bounds. Derive from the live Z extent of the store (one O(N)
	// pass - acceptable once at gather; the legacy did the same off the sorted array).
	MinHeight = -100.f;
	MaxHeight = 100.f;
	bool bAny = false;
	float MinZ = TNumericLimits<float>::Max();
	float MaxZ = -TNumericLimits<float>::Max();
	BuildingStore.ForEach([&](FBuildingHandle, const FBuildingRecord& Record)
	{
		bAny = true;
		MinZ = FMath::Min(MinZ, Record.Pos.Z);
		MaxZ = FMath::Max(MaxZ, Record.Pos.Z);
	});
	if (bAny)
	{
		MinHeight = MinZ;
		MaxHeight = MaxZ;
	}

	// Apply the current slider range to the compositor (marks dirty for the new Z range).
	OnZFilterUpdated(MinCached, MaxCached);

	CARTO_LOG("StreamingGather Finished. Live: %d", BuildingStore.Num());

	IsInitializing = false;

	// First-paint on join, like the ORIGINAL mod (which redrew on data-change and on the
	// gather-finished path). SetFullRedraw marks every populated tile dirty; the
	// never-cancelled compositor does NOT draw it immediately - it DEBOUNCES on the
	// TileManager dirty epoch and only drains once the stream-in has settled
	// (r.Cartograph.DrainDebounceTicks), so the converged atlas is painted ONCE in O(N).
	// We do NOT gate on the map being open: the v2.0.3/4 OnVanillaMapMenuShown
	// (Hook_MapMenu_Cartograph) hook NEVER fires on this SDK, so the gate never opened and
	// the atlas was never painted. The debounce is what prevents the O(N^2) churn/OOM, not
	// a map-open gate.
	if (!bIsDedicatedServer)
	{
		Compositor.SetFullRedraw();
	}
}


// =============================================================================
// Server net-tick: repack dirty tiles + push deltas to AoI clients.
// =============================================================================
void UCartographGameInstanceModule::ServerNetTick()
{
	if (IsClient || !bSpineInitialized)
	{
		return;  // clients never repack; host/server only
	}

	// Push per-client deltas. Each PC's UCartographMapReplicationComponent diffs the
	// live per-tile versions against what it last sent that client and streams a
	// shallow window (SPEC 4.4). On a listen host with no remote clients this is a
	// no-op. Repacking is lazy inside GetTileSnapshot, so we do not eagerly repack
	// every dirty tile here.
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (APlayerController* PC = It->Get())
		{
			if (UCartographMapReplicationComponent* ReplComp = PC->FindComponentByClass<UCartographMapReplicationComponent>())
			{
				ReplComp->PushPendingDeltas();
			}
		}
	}
}


#pragma region UI
void UCartographGameInstanceModule::RegisterMenuButton() const
{
	if (!FPlatformProperties::RequiresCookedData() || FPlatformProperties::IsServerOnly())
	{
		return;
	}

	const auto* WidgetBlueprintClass = Cast<UWidgetBlueprintGeneratedClass>(MapContainerWidget.LoadSynchronous());
	UWidgetTree* WidgetTree = WidgetBlueprintClass->GetWidgetTreeArchetype();

	const UWidget* Menu = WidgetTree->FindWidget("BPW_MapMenu");
	CARTO_LOG_ERROR_RETURN_IF_NULL(Menu);
	const auto* MenuPanelSlot = Cast<UCanvasPanelSlot>(Menu->Slot);
	CARTO_LOG_ERROR_RETURN_IF_NULL(MenuPanelSlot);

	UWidget* ShowHideButton = WidgetTree->FindWidget("ShowHideButton");
    CARTO_LOG_ERROR_RETURN_IF_NULL(ShowHideButton);

	// Remove the Show/Hide menu button from its parent, as we'll be replacing that with a hbox
	int32 Index;
	UPanelWidget* Parent = UWidgetTree::FindWidgetParent(ShowHideButton, Index);
	CARTO_LOG_ERROR_RETURN_IF_NULL(Parent);
	Parent->RemoveChild(ShowHideButton);

	// Create a new hox and add it to the parent, maintaining the original index
    UHorizontalBox* HBox = NewObject<UHorizontalBox>(WidgetTree, UHorizontalBox::StaticClass(), "MenuShowHideButtonHBox", RF_Transient);
	auto* HBoxPanelSlot = Cast<UCanvasPanelSlot>(Parent->InsertChildAt(Index, HBox));
	HBoxPanelSlot->SetPosition({ 6, 6 });
	HBoxPanelSlot->SetAutoSize(true);

	// Add the Show/Hide menu button to the hbox
	HBox->AddChild(ShowHideButton);

	// Create Cartograph's Show/Hide menu button and add it to the hbox
	UWidget* CartographMenuShowHideButton = WidgetTree->ConstructWidget<UWidget>(MenuShowHideButtonWidget, "CartographMenuShowHideButton");
	auto* HBoxSlot = Cast<UHorizontalBoxSlot>(HBox->AddChild(CartographMenuShowHideButton));
	HBoxSlot->SetPadding({ 10, 0, 0, 0 });

	// Update the button's text
	FProperty* TextProperty = CartographMenuShowHideButton->GetClass()->FindPropertyByName("mText");
    CARTO_LOG_ERROR_RETURN_IF_NULL(TextProperty);
	FText* TextPtr = TextProperty->ContainerPtrToValuePtr<FText>(CartographMenuShowHideButton);
    CARTO_LOG_ERROR_RETURN_IF_NULL(TextPtr);
    *TextPtr = LOCTEXT("CartographMenuShow", "Show Cartograph Menu");


	// Now create the actual menu
	UWidget* CartographMenu = NewObject<UWidget>(HBox, MenuWidget, "CartographMenu", RF_Transient);
	auto* CartographMenuPanelSlot = Cast<UCanvasPanelSlot>(Parent->AddChild(CartographMenu));
    CartographMenuPanelSlot->SetLayout(MenuPanelSlot->GetLayout());
    CartographMenuPanelSlot->SetPosition(MenuPanelSlot->GetPosition());
    CartographMenuPanelSlot->SetSize(MenuPanelSlot->GetSize());
    CartographMenuPanelSlot->SetAutoSize(MenuPanelSlot->GetAutoSize());
    CartographMenuPanelSlot->SetZOrder(MenuPanelSlot->GetZOrder());

	CartographMenu->SetVisibility(ESlateVisibility::Collapsed);

    CARTO_LOG("Menu Button Registered");
}


void UCartographGameInstanceModule::LoadRuntimeConfig()
{
#if !UE_SERVER
	const FCartograph_ConfigStruct ConfigInstance = FCartograph_ConfigStruct::GetActiveConfig(GetWorld());

	RuntimeConfig = {};

	// Parse the comma/pipe/colon-separated config strings with UE-native FString
	// ops. The previous std::wstringstream/std::getline path only compiled on
	// Windows, where TCHAR == wchar_t; on Linux TCHAR == char16_t, so a
	// std::wstringstream (wchar_t) cannot be constructed from *FString.
	{
		TArray<FString> Categories;
		ConfigInstance.MainCategoryToggle.ParseIntoArray(Categories, TEXT(","), /*CullEmpty*/ true);
		for (const FString& Category : Categories)
		{
			RuntimeConfig.DisabledLayerMainCategory.Add(FName{ Category });
		}
	}
	{
		TArray<FString> MainCategoryLines;
		ConfigInstance.SubCategoryToggle.ParseIntoArray(MainCategoryLines, TEXT("|"), /*CullEmpty*/ true);
		for (const FString& MainCategoryLine : MainCategoryLines)
		{
			FString MainCategoryName, SubCategoryList;
			if (!MainCategoryLine.Split(TEXT(":"), &MainCategoryName, &SubCategoryList))
			{
				CARTO_LOG_ERROR("Invalid SubCategoryToggle: %s", *ConfigInstance.SubCategoryToggle);
				break;
			}
			const FName MainCategoryFName{ MainCategoryName };

			TArray<FString> SubCategories;
			SubCategoryList.ParseIntoArray(SubCategories, TEXT(","), /*CullEmpty*/ true);
			for (const FString& SubCategory : SubCategories)
			{
				RuntimeConfig.DisabledLayerSubCategory.FindOrAdd(MainCategoryFName).Add(FName{ SubCategory });
			}
		}
	}

	{
		TArray<FString> Buildings;
		ConfigInstance.BuildingToggle.ParseIntoArray(Buildings, TEXT(","), /*CullEmpty*/ true);
		for (const FString& Building : Buildings)
		{
			RuntimeConfig.DisabledLayerBuildable.Add(static_cast<uint32>(FCString::Strtoui64(*Building, nullptr, 10)));
		}
	}

	CARTO_LOG_DEBUG("RuntimeConfig Loaded");
#endif
}


void UCartographGameInstanceModule::SaveRuntimeConfig()
{
#if !UE_SERVER
	const FConfigId ConfigId{ "Cartograph", "" };
	const UConfigManager* ConfigManager = GetWorld()->GetGameInstance()->GetSubsystem<UConfigManager>();
	const UConfigPropertySection* RootSection = ConfigManager->GetConfigurationRootSection(ConfigId);

	{
		const TObjectPtr<UConfigProperty>* MainCategoryProperty = RootSection->SectionProperties.Find("MainCategoryToggle");
		CARTO_LOG_ERROR_RETURN_IF_NULL(MainCategoryProperty);
		auto* MainCategoryStringProperty = Cast<UConfigPropertyString>(*MainCategoryProperty);
		CARTO_LOG_ERROR_RETURN_IF_NULL(MainCategoryStringProperty);

        TStringBuilder<500> Builder;
        for (const FName& Name : RuntimeConfig.DisabledLayerMainCategory)
        {
			Builder.Appendf(TEXT("%s,"), *Name.ToString());
        }
        MainCategoryStringProperty->Value = Builder.ToString();
		MainCategoryStringProperty->MarkDirty();
	}
	{
		const TObjectPtr<UConfigProperty>* SubCategoryProperty = RootSection->SectionProperties.Find("SubCategoryToggle");
        CARTO_LOG_ERROR_RETURN_IF_NULL(SubCategoryProperty);
        auto* SubCategoryStringProperty = Cast<UConfigPropertyString>(*SubCategoryProperty);
        CARTO_LOG_ERROR_RETURN_IF_NULL(SubCategoryStringProperty);
        TStringBuilder<1000> Builder;
        for (const auto& [MainCategory, SubCategories] : RuntimeConfig.DisabledLayerSubCategory)
        {
            Builder.Appendf(TEXT("%s:"), *MainCategory.ToString());
            for (const FName& Name : SubCategories)
            {
				Builder.Appendf(TEXT("%s,"), *Name.ToString());
            }
            Builder.Append(TEXT("|"));
        }
        SubCategoryStringProperty->Value = Builder.ToString();
        SubCategoryStringProperty->MarkDirty();
    }
    {
	    const TObjectPtr<UConfigProperty>* BuildingProperty = RootSection->SectionProperties.Find("BuildingToggle");
        CARTO_LOG_ERROR_RETURN_IF_NULL(BuildingProperty);
        auto* BuildingStringProperty = Cast<UConfigPropertyString>(*BuildingProperty);
        CARTO_LOG_ERROR_RETURN_IF_NULL(BuildingStringProperty);
        TStringBuilder<11 * 551> Builder;
        for (const uint32& Hash : RuntimeConfig.DisabledLayerBuildable)
        {
			Builder.Appendf(TEXT("%u,"), Hash);
        }
        BuildingStringProperty->Value = Builder.ToString();
        BuildingStringProperty->MarkDirty();
	}

	CARTO_LOG_DEBUG("RuntimeConfig Saved");
#endif
}


void UCartographGameInstanceModule::FillBuildLayerDataCache()
{
	if (!BuildLayerDataMapCache.IsEmpty())
	{
		return;
	}

	for (const auto& [OriginalBuildableClass, _] : ClassPtrToClassIDMap)
	{
		if (!OriginalBuildableClass || BuildableToIgnore.Contains(OriginalBuildableClass.Get()))
		{
			continue;
		}

		// Build the soft-class key explicitly so Find hashes/compares over the soft
		// object path (the map's true key identity), not via an implicit-and-maybe-
		// explicit UClass* -> TSoftClassPtr conversion inside Find.
		const TSoftClassPtr<AFGBuildable>* RedirectClass = BuildableClassRedirectMap.Find(TSoftClassPtr<AFGBuildable>(OriginalBuildableClass.Get()));
		const TSubclassOf<AFGBuildable> BuildableClass = RedirectClass ? RedirectClass->LoadSynchronous() : OriginalBuildableClass.Get();

		const uint32* BuildableClassHash = ClassPtrToClassIDMap.Find(BuildableClass);
		if (!BuildableClassHash)
		{
			CARTO_LOG_ERROR("Can't find hash for %s", *BuildableClass->GetName());
			continue;
		}


        const FBuildLayerData* LayerData = GetDataByBuildableClass(BuildableBuildLayerDataOverrideMap, BuildLayerDataMap, BuildableClass);
		if (!LayerData && ModdedBuildings.Contains(BuildableClass.Get()))
		{
			const FString& ModName = *ModdedBuildings.Find(BuildableClass.Get());
            LayerData = ModdedBuildLayerData.Find(ModName);
		}
		if (!LayerData)
		{
            CARTO_LOG_WARNING("Can't find layer data for %s", *BuildableClass->GetName());
            continue;
		}

		if (LayerData->MainCategoryCache.IsNone())
		{
			TArray<FString> CategoryNames;
			LayerData->Category.ParseIntoArray(CategoryNames, TEXT("/"));
            if (CategoryNames.Num() == 0)
            {
                CARTO_LOG_ERROR("Invalid Category: %s", *LayerData->Category);
                continue;
            }

			const_cast<FBuildLayerData*>(LayerData)->MainCategoryCache = FName{ CategoryNames[0] };
			const_cast<FBuildLayerData*>(LayerData)->SubCategoryCache = CategoryNames.Num() > 1 ? FName{ CategoryNames[1] } : NAME_None;
		}

		BuildLayerDataMapCache.Add(*BuildableClassHash, LayerData);
	}

    CARTO_LOG("BuildLayerDataCache Filled. Count: %d", BuildLayerDataMapCache.Num());
}


void UCartographGameInstanceModule::OnZFilterUpdated(float Min, float Max)
{
	MinCached = Min;
    MaxCached = Max;

	const float Length = MaxHeight - MinHeight;
	const float MinZFilter = FMath::Floor(Min * Length + MinHeight);
	const float MaxZFilter = FMath::CeilToInt(Max * Length + MinHeight);

	CARTO_LOG("Min is now %f and max is now %f", MinZFilter, MaxZFilter);

	// Z-slider is now O(bands + hits): the compositor re-converges the affected tiles
	// instead of the legacy O(N) re-walk + 256 MB clear (client-full-rebuild-on-zfilter).
	if (!bIsDedicatedServer)
	{
		Compositor.SetZFilter(MinZFilter, MaxZFilter);
	}
}


void UCartographGameInstanceModule::OnCartographMenuButtonClicked(UUserWidget* Widget, bool IsOpen)
{
	for (UWidget* ChildWidget : Widget->GetParent()->GetParent()->GetAllChildren())
	{
		if (ChildWidget->GetName() == "CartographMenu")
		{
			ChildWidget->SetVisibility(IsOpen ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
			break;
		}
	}

	// NOTE: the compositor drain is NOT gated on any map-open signal. It renders on
	// data-change (build hooks -> MarkDirtyForBox) and on join (SetFullRedraw), debounced
	// on the TileManager dirty epoch so a megabase stream-in paints once in O(N). This
	// handler only toggles the Cartograph filters submenu visibility.

	if (IsOpen)
	{
		auto* RootWidget = Cast<UWidget>(Widget->GetParent()->GetOuter()->GetOuter());
        CARTO_LOG_ERROR_RETURN_IF_NULL(RootWidget);
		FOutputDeviceNull Ar;
		RootWidget->CallFunctionByNameWithArguments(TEXT("SetFiltersCollapsed 1"), Ar, nullptr, true);
	}
}


void UCartographGameInstanceModule::OnShowBuildingsCheckboxChanged(bool DoShow)
{
    DoShowBuildings = DoShow;
    CARTO_LOG("DoShowBuildings: %d", DoShowBuildings);

	// Toggle building visibility on the compositor (re-converges per tile).
	if (!bIsDedicatedServer)
	{
		Compositor.SetShowBuildings(DoShow);
	}
}


TArray<FString> UCartographGameInstanceModule::GetLayerCategoryOptions() const
{
	TArray<FString> Options;
	for (const FLayerCategoryData& LayerCategory : LayerCategories)
	{
		Options.Add(LayerCategory.Name.ToString());

		for (const FLayerSubCategoryData& LayerSubCategory : LayerCategory.SubCategories)
		{
			Options.Add(FString::Printf(TEXT("%s/%s"), *LayerCategory.Name.ToString(), *LayerSubCategory.Name.ToString()));
		}
	}
	return Options;
}


void UCartographGameInstanceModule::OnVanillaMapMenuShown(const UUserWidget* Widget) const
{
	// Fired by the SML Hook_MapMenu_Cartograph hook (Widget_MapContainer_C) when the
	// vanilla map opens. This BP asset binds to this UFUNCTION BY NAME, so the function
	// MUST exist or the asset fails to cook - even though, on the current SDK, the hook
	// does not actually fire at runtime. It does ONLY the original UI bookkeeping (start
	// the Cartograph filters submenu collapsed); it deliberately does NOT drive the
	// compositor. Rendering is data-driven + debounced in the compositor itself, NOT
	// gated on map-open (that gate never worked - see TickConverge).
	UWidget* Menu = Widget->WidgetTree->FindWidget("CartographMenu");
	CARTO_LOG_ERROR_RETURN_IF_NULL(Menu);
	Menu->SetVisibility(ESlateVisibility::Collapsed);

	UWidget* Button = Widget->WidgetTree->FindWidget("CartographMenuShowHideButton");
	CARTO_LOG_ERROR_RETURN_IF_NULL(Button);

	FProperty* IsOpenProperty = Button->GetClass()->FindPropertyByName("IsOpen");
	CARTO_LOG_ERROR_RETURN_IF_NULL(IsOpenProperty);
	*IsOpenProperty->ContainerPtrToValuePtr<bool>(Button) = false;

	FOutputDeviceNull Ar;
	Button->CallFunctionByNameWithArguments(TEXT("SetShowHideText"), Ar, nullptr, true);
}


#pragma endregion


void UCartographGameInstanceModule::GatherBuildables()
{
    ClassIDToClassPtrMap.Empty();
    ClassPtrToClassIDMap.Empty();
    ClassPtrToDescriptorDataMap.Empty();

	for (const FTopLevelAssetPath& AssetPath : GetDerivedClassPaths(AFGBuildable::StaticClass()))
	{
		const TSubclassOf<AFGBuildable> Class = StaticLoadClass(AFGBuildable::StaticClass(), nullptr, *AssetPath.ToString());
        if (!Class)
        {
            CARTO_LOG_ERROR("Failed to load class from path: %s", *AssetPath.ToString());
            continue;
        }

		const FString Name = Class->GetName();
		const FString PackageName = AssetPath.GetPackageName().ToString();
		if (PackageName.StartsWith("/Script")
			|| Name.StartsWith("SKEL_") || Name.StartsWith("REINST_"))
		{
			continue;
		}

		if (!PackageName.StartsWith("/Game/"))
		{
			const int32 Index = PackageName.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromStart, 2);
			const FString ModName = PackageName.Mid(1, Index - 1);
			ModdedBuildings.Add(Class.Get(), ModName);
			FLayerCategoryData* ModdedCategory = LayerCategories.FindByPredicate(
				[](const FLayerCategoryData& CategoryData) { return CategoryData.Name == UnspecifiedMainCategory; });
			CARTO_LOG_ERROR_DO_IF_NULL(ModdedCategory, continue);
            if (!ModdedCategory->SubCategories.FindByPredicate(
				[ModFName = FName{ ModName }](const FLayerSubCategoryData& CategoryData) { return CategoryData.Name == ModFName; }))
            {
                ModdedCategory->SubCategories.Add(FLayerSubCategoryData{
                    .Name = FName{ ModName },
                    .DisplayName = FText::FromString(ModName),
					});
            }

			if (!ModdedBuildLayerData.Contains(ModName))
			{
				ModdedBuildLayerData.Add(ModName, FBuildLayerData{
					.MainCategoryCache = FName{ UnspecifiedMainCategory },
					.SubCategoryCache = FName{ ModName },
					});
			}
		}

		const uint32 Hash = TextKeyUtil::HashString(AssetPath.ToString());
		ClassPtrToClassIDMap.Add(Class, Hash);
		ClassIDToClassPtrMap.Add(Hash, Class);
		CARTO_LOG_DEBUG("Path: %s, Class: %s, Hash: %u", *AssetPath.ToString(), *Name, Hash);
	}
	for (const FTopLevelAssetPath& AssetPath : GetDerivedClassPaths(UFGBuildingDescriptor::StaticClass()))
	{
		const TSubclassOf<UFGBuildingDescriptor> Descriptor = StaticLoadClass(UFGBuildingDescriptor::StaticClass(), nullptr, *AssetPath.ToString());
        if (!Descriptor)
        {
            CARTO_LOG_ERROR("Failed to load descriptor from path: %s", *AssetPath.ToString());
            continue;
        }

		const FString Name = Descriptor->GetName();
		const FString PackageName = AssetPath.GetPackageName().ToString();
		if (PackageName.StartsWith("/Script")
			|| Name.StartsWith("SKEL_") || Name.StartsWith("REINST_"))
		{
			continue;
		}

		const auto* DescriptorInstance = GetDefault<UFGBuildingDescriptor>(Descriptor);
		CARTO_LOG_ERROR_DO_IF_NULL(DescriptorInstance, continue);
		TSubclassOf<AFGBuildable> BuildableClass = DescriptorInstance->mBuildableClass;
		if (!BuildableClass)
		{
			continue;
		}

		TSubclassOf<UFGBuildSubCategory> BuildSubCategory;
		for (const TSubclassOf<UFGCategory>& SubCategory : DescriptorInstance->mSubCategories)
		{
            if (!SubCategory)
            {
                continue;
            }

			if (SubCategory->IsChildOf(UFGBuildSubCategory::StaticClass()))
			{
				BuildSubCategory = SubCategory;
				break;
			}
		}

        UTexture2D* Icon = DescriptorInstance->mSmallIcon;
		if (!Icon)
		{
            // Some buildings like blueprint designers don't have small icon
			Icon = DescriptorInstance->mPersistentBigIcon;
		}

        ClassPtrToDescriptorDataMap.Add(BuildableClass, FBuildingDescriptorData{
			.Category = DescriptorInstance->mCategory,
			.SubCategory = BuildSubCategory,
			.Icon = Icon,
        });

		CARTO_LOG_DEBUG("Path: %s, Class: %s, BuildableClass: %s, NoIcon: %d",
			*AssetPath.ToString(),
			*Name,
			*BuildableClass->GetName(),
			Icon == nullptr);
	}

    CARTO_LOG("Buildables Gathered. Buildable: %d, Descriptor: %d", ClassPtrToClassIDMap.Num(), ClassPtrToDescriptorDataMap.Num());
}


template<typename T>
concept HasStaticStruct = requires
{
	T::StaticStruct();
};


template<typename T>
void UCartographGameInstanceModule::FillInMatchingProperties(const FProperty* StructPropertyToCompare, TArray<std::pair<const FProperty*, const FProperty*>>& Out)
{
	static_assert(HasStaticStruct<T>);

	const auto* StructProperty = CastField<FStructProperty>(StructPropertyToCompare);
	if (!StructProperty)
	{
		CARTO_LOG_ERROR("Property type should be a struct!");
		return;
	}

	for (TFieldIterator<FProperty> PropertyIt{ T::StaticStruct(), EFieldIteratorFlags::IncludeSuper }; PropertyIt; ++PropertyIt)
	{
		// FindPropertyByName doesn't work here, they're stored as like "Category_2_1FC60E30497F81318BD45A8E6799F144"
		const FProperty* OverrideStructProperty = StructProperty->Struct->CustomFindProperty(PropertyIt->GetFName());
		if (!OverrideStructProperty)
		{
			continue;
		}
		CARTO_LOG("Found struct property: %s", *OverrideStructProperty->GetAuthoredName());

		if (!OverrideStructProperty->SameType(*PropertyIt))
		{
			CARTO_LOG_ERROR("Struct property type is wrong");
			continue;
		}

		Out.Add({ *PropertyIt, OverrideStructProperty });
	}
}


template<typename KeyType, typename ValueType>
void UCartographGameInstanceModule::ProcessOverrideData(TMap<KeyType, ValueType>& MapToBeOverriden, UClass* OverrideDataClass, FName PropertyName)
{
	const FProperty* ThisProperty = GetClass()->FindPropertyByName(PropertyName);
	CARTO_LOG_ERROR_RETURN_IF_NULL(ThisProperty);

	const auto* ThisMapProperty = CastField<const FMapProperty>(ThisProperty);
	CARTO_LOG_ERROR_RETURN_IF_NULL(ThisMapProperty);


	const FProperty* Property = OverrideDataClass->FindPropertyByName(PropertyName);
	if (!Property)
	{
		return;
	}

    CARTO_LOG("Found Property: %s", *PropertyName.ToString());

	const auto* MapProperty = CastField<const FMapProperty>(Property);
	if (!MapProperty)
	{
		CARTO_LOG_ERROR("It should be a map!");
		return;
	}

	if (!MapProperty->KeyProp->SameType(ThisMapProperty->KeyProp))
	{
        CARTO_LOG_ERROR("Key type is wrong");
        return;
	}

    TArray<std::pair<const FProperty*, const FProperty*>> MatchingValueStructProperties;
    if constexpr (!HasStaticStruct<ValueType>)
    {
        if (!MapProperty->ValueProp->SameType(ThisMapProperty->ValueProp))
        {
            CARTO_LOG_ERROR("Value type is wrong");
            return;
        }
    }
	else
	{
        FillInMatchingProperties<ValueType>(MapProperty->ValueProp, MatchingValueStructProperties);
		if (MatchingValueStructProperties.Num() == 0)
		{
			return;
		}
	}


	auto* CDO = GetMutableDefault<UObject>(OverrideDataClass);
	CARTO_LOG_ERROR_RETURN_IF_NULL(CDO);
	MapProperty->WithScriptMap(Property->ContainerPtrToValuePtr<void>(CDO),
		[this, MapProperty, &MatchingValueStructProperties, &MapToBeOverriden](auto* OverrideMap)
		{
			const int32 Num = OverrideMap->Num();
			for (int32 i = 0; i < Num; i++)
			{
				const uint8* KeyPtr = static_cast<uint8*>(OverrideMap->GetData(i, MapProperty->MapLayout));
				const uint8* ValuePtr = KeyPtr + MapProperty->MapLayout.ValueOffset;

				const KeyType& Key = *reinterpret_cast<const KeyType*>(KeyPtr);
				ValueType& OverrideValue = MapToBeOverriden.FindOrAdd(Key);

				if constexpr (!HasStaticStruct<ValueType>)
				{
                    OverrideValue = *reinterpret_cast<const ValueType*>(ValuePtr);
				}
				else
				{
                    for (const auto& [OriginalStructProperty, OverrideStructProperty] : MatchingValueStructProperties)
                    {
                        auto* StructPropertyValue = OverrideStructProperty->ContainerPtrToValuePtr<void>(ValuePtr);
                        OverrideStructProperty->CopyCompleteValue(OriginalStructProperty->ContainerPtrToValuePtr<void>(&OverrideValue), StructPropertyValue);
                    }
				}
			}
            CARTO_LOG("Added %d elements", Num);
		}
	);
}


template<typename T>
void UCartographGameInstanceModule::ProcessOverrideData(TSet<T>& SetToBeOverriden, UClass* OverrideDataClass, FName PropertyName)
{
	static_assert(!HasStaticStruct<T>, "Didn't bother to implement");

	const FProperty* ThisProperty = GetClass()->FindPropertyByName(PropertyName);
	CARTO_LOG_ERROR_RETURN_IF_NULL(ThisProperty);

	const auto* ThisSetProperty = CastField<const FSetProperty>(ThisProperty);
	CARTO_LOG_ERROR_RETURN_IF_NULL(ThisSetProperty);


	const FProperty* Property = OverrideDataClass->FindPropertyByName(PropertyName);
	if (!Property)
	{
		return;
	}

	CARTO_LOG("Found Property: %s", *PropertyName.ToString());

	const auto* SetProperty = CastField<const FSetProperty>(Property);
	if (!SetProperty)
	{
		CARTO_LOG_ERROR("It should be a set!");
		return;
	}

	if (!SetProperty->ElementProp->SameType(ThisSetProperty->ElementProp))
	{
		CARTO_LOG_ERROR("Element type is wrong");
		return;
	}


	auto* CDO = GetMutableDefault<UObject>(OverrideDataClass);
	CARTO_LOG_ERROR_RETURN_IF_NULL(CDO);
	void* OverrideSet = Property->ContainerPtrToValuePtr<void>(CDO);
	const int32 Num = SetProperty->GetNum(OverrideSet);
    for (int32 i = 0; i < Num; i++)
    {
        const uint8* ElementPtr = SetProperty->GetElementPtr(OverrideSet, i);
        const T& Element = *reinterpret_cast<const T*>(ElementPtr);
        SetToBeOverriden.Add(Element);
    }
    CARTO_LOG("Added %d elements", Num);
}


void UCartographGameInstanceModule::ProcessLayerCategoriesOverride(UClass* OverrideDataClass)
{
	const FProperty* ThisProperty = GetClass()->FindPropertyByName("LayerCategories");
	CARTO_LOG_ERROR_RETURN_IF_NULL(ThisProperty);

	const auto* ThisArrayProperty = CastField<const FArrayProperty>(ThisProperty);
	CARTO_LOG_ERROR_RETURN_IF_NULL(ThisArrayProperty);


	const FProperty* Property = OverrideDataClass->FindPropertyByName("LayerCategories");
	if (!Property)
	{
		return;
	}

	CARTO_LOG("Found Property: LayerCategories");

	const auto* ArrayProperty = CastField<const FArrayProperty>(Property);
	if (!ArrayProperty)
	{
		CARTO_LOG_ERROR("It should be an array!");
		return;
	}

	const auto* StructProperty = CastField<FStructProperty>(ArrayProperty->Inner);
	if (!StructProperty)
	{
		CARTO_LOG_ERROR("Property type should be a struct!");
		return;
	}

	TArray<std::pair<const FProperty*, const FProperty*>> MatchingValueStructProperties;
    FillInMatchingProperties<FLayerSubCategoryData>(ArrayProperty->Inner, MatchingValueStructProperties);
	if (MatchingValueStructProperties.Num() == 0)
	{
		return;
	}

	const FArrayProperty* SubCategoriesArrayProperty = nullptr;
	TArray<std::pair<const FProperty*, const FProperty*>> MatchingSubCategoriesStructProperties;
	if (const FProperty* SubCategoriesProperty = StructProperty->Struct->CustomFindProperty("SubCategories"))
	{
		CARTO_LOG("Found struct property: SubCategories");

        SubCategoriesArrayProperty = CastField<FArrayProperty>(SubCategoriesProperty);
        if (!SubCategoriesArrayProperty)
        {
            CARTO_LOG_ERROR("It should be an array!");
        }
		else
		{
            FillInMatchingProperties<FLayerSubCategoryData>(SubCategoriesArrayProperty->Inner, MatchingSubCategoriesStructProperties);
		}
	}


	auto* CDO = GetMutableDefault<UObject>(OverrideDataClass);
	CARTO_LOG_ERROR_RETURN_IF_NULL(CDO);
    void* OverrideArray = Property->ContainerPtrToValuePtr<void>(CDO);
    const int32 Num = FScriptArrayHelper{ ArrayProperty, OverrideArray }.Num();
    CARTO_LOG("Categories: %d", Num);
    for (int32 i = 0; i < Num; i++)
    {
		void* ElementPtr = ArrayProperty->GetValueAddressAtIndex_Direct(ArrayProperty->Inner, OverrideArray, i);
		FLayerCategoryData& OverrideValue = LayerCategories.AddDefaulted_GetRef();

		for (const auto& [OriginalStructProperty, OverrideStructProperty] : MatchingValueStructProperties)
		{
			auto* StructPropertyValue = OverrideStructProperty->ContainerPtrToValuePtr<void>(ElementPtr);
			OverrideStructProperty->CopyCompleteValue(OriginalStructProperty->ContainerPtrToValuePtr<void>(&OverrideValue), StructPropertyValue);
		}

		CARTO_LOG("MainCategory #%d", i);
        CARTO_LOG("Name: %s", *OverrideValue.Name.ToString());
        CARTO_LOG("DisplayName: %s", *OverrideValue.DisplayName.ToString());
        CARTO_LOG("Priority: %d", OverrideValue.Priority);

        if (!SubCategoriesArrayProperty)
        {
			continue;
        }

		void* SubCategoriesArray = SubCategoriesArrayProperty->ContainerPtrToValuePtr<void>(ElementPtr);
        const int32 SubCategoryNum = FScriptArrayHelper{ SubCategoriesArrayProperty, SubCategoriesArray }.Num();
		CARTO_LOG("SubCategories: %d", SubCategoryNum);
    	for (int32 j = 0; j < SubCategoryNum; j++)
		{
			const void* SubCategoriesElementPtr = SubCategoriesArrayProperty->GetValueAddressAtIndex_Direct(SubCategoriesArrayProperty->Inner, SubCategoriesArray, j);
			FLayerSubCategoryData& SubCategoryData = OverrideValue.SubCategories.AddDefaulted_GetRef();

			for (const auto& [OriginalStructProperty, OverrideStructProperty] : MatchingSubCategoriesStructProperties)
			{
				auto* StructPropertyValue = OverrideStructProperty->ContainerPtrToValuePtr<void>(SubCategoriesElementPtr);
				OverrideStructProperty->CopyCompleteValue(OriginalStructProperty->ContainerPtrToValuePtr<void>(&SubCategoryData), StructPropertyValue);
			}

            CARTO_LOG("SubCategory #%d", j);
            CARTO_LOG("Name: %s", *SubCategoryData.Name.ToString());
            CARTO_LOG("DisplayName: %s", *SubCategoryData.DisplayName.ToString());
            CARTO_LOG("Priority: %d", SubCategoryData.Priority);
		}
    }
}


void UCartographGameInstanceModule::GatherModOverrides()
{
	TArray<FAssetData> OverrideAssetArray;
	const FName OverrideDataFileName{ "CartographOverrideData" };

	TBaseStructure<FVector>::Get();
	TBaseStructure<FCategoryData>::Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	IAssetRegistry::Get()->EnumerateAssets(Filter,
		[&OverrideAssetArray, &OverrideDataFileName](const FAssetData& Asset)
		{
			if (Asset.AssetName == OverrideDataFileName)
			{
				OverrideAssetArray.Add(Asset);
			}

			return true;
		});

#define VAR(x) std::make_pair(std::ref(x), FName{ #x })
	const auto OverrideableVariables = std::make_tuple(
		VAR(BuildCategoryDataMap), VAR(BuildableBuildCategoryDataOverrideMap), VAR(MaterialBuildCategoryDataOverrideMap),
		VAR(BuildableIconOverrideMap), VAR(BuildableSizeOverrideMap), VAR(BuildableExtraRotationMap),
		VAR(BuildableSplineDataMap), VAR(BuildableWireDataMap),
		VAR(BuildableClassRedirectMap), VAR(BuildableToIgnore),
		/*VAR(LayerCategories),*/ VAR(BuildLayerDataMap), VAR(BuildableBuildLayerDataOverrideMap), VAR(MaterialBuildLayerDataOverrideMap));
#undef VAR


	for (const FAssetData& OverrideAssetData : OverrideAssetArray)
	{
		CARTO_LOG("Override Data Found: %s", *OverrideAssetData.PackageName.ToString());

		const FString OverrideDataClassName = OverrideAssetData.GetObjectPathString() + TEXT("_C");
		UClass* OverrideDataClass = LoadObject<UClass>(nullptr, *OverrideDataClassName);
		CARTO_LOG_ERROR_DO_IF_NULL(OverrideDataClass, continue);

		const auto LambdaProcessOverrideData =
			[this, OverrideDataClass, &OverrideableVariables]<size_t Index>()
			{
				auto& [VariableRef, Name] = std::get<Index>(OverrideableVariables);
				ProcessOverrideData(VariableRef, OverrideDataClass, Name);
			};

		[&LambdaProcessOverrideData]<size_t ...Index>(std::index_sequence<Index...>)
		{
			(LambdaProcessOverrideData.template operator()<Index>(), ...);
		}(std::make_index_sequence<std::tuple_size_v<decltype(OverrideableVariables)>>{});
		ProcessLayerCategoriesOverride(OverrideDataClass);
	}
}


TSet<FTopLevelAssetPath> UCartographGameInstanceModule::GetDerivedClassPaths(UClass* ParentClass)
{
	TArray<UClass*> NativeRootClasses;
	NativeRootClasses.Add(ParentClass);
	GetDerivedClasses(ParentClass, NativeRootClasses);

	TArray<FTopLevelAssetPath> NativeRootClassPaths;

	Algo::TransformIf(NativeRootClasses,
		NativeRootClassPaths,
		[](const UClass* RootClass) { return RootClass && RootClass->HasAnyClassFlags(CLASS_Native); },
		&UClass::GetClassPathName);

	TSet<FTopLevelAssetPath> AllClassPaths;
	IAssetRegistry::Get()->GetDerivedClassNames(NativeRootClassPaths, {}, AllClassPaths);

	return AllClassPaths;
}


#undef LOCTEXT_NAMESPACE
