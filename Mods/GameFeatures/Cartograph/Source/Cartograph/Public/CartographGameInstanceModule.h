#pragma once

#include <array>

#include "CoreMinimal.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Module/GameInstanceModule.h"

#include "FGBuildSubCategory.h"
#include "FGLightweightBuildableSubsystem.h"

#include "UE5Coro/UE5Coro.h"

// World bounds, RENDER_TEXTURE_SIZE, ORIGIN_UV, PIXEL_PER_CENTIMETER, SPLINE_SEGMENTS,
// grid/tile/Z-band geometry and the render-backend CVars now live in the FROZEN
// CartographConfig.h (SPEC). The legacy duplicates that used to be declared HERE
// (with a DIFFERENT RENDER_TEXTURE_SIZE = 8192) were an ODR/redefinition conflict
// with CartographConfig.h's 4096 in any TU that included both (CartographClassDrawTable.cpp,
// CartographCompositor.cpp, CartographMapReplicator.cpp all do). They are deleted
// here and this header now includes the single authority. (Integration reconcile.)
#include "CartographConfig.h"

// The slim spine + render/net subsystems owned by this module post-rearchitecture.
#include "Core/CartographTypes.h"
#include "Core/CartographBuildingStore.h"
#include "Core/CartographSpatialGrid.h"
#include "Core/CartographZBandIndex.h"
#include "Core/CartographClassDrawTable.h"
#include "Core/CartographTileManager.h"
#include "Render/CartographCompositor.h"
#include "Net/CartographMapReplicator.h"

#include "CartographGameInstanceModule.generated.h"


class AFGBuildable;
class AFGBuildableSubsystem;
class AFGLightweightBuildableSubsystem;
class UCanvasRenderTarget2D;
class UFGBuildCategory;
class UFGBuildSubCategory;
class UFGBuildingDescriptor;
struct FRuntimeBuildableInstanceData;


// LogCartograph is declared in CartographConfig.h (included above) so it is
// declared exactly once across all TUs; defined in CartographGameInstanceModule.cpp.


constexpr bool ENABLE_DEBUG_LOG = false;
constexpr bool ENABLE_VERBOSE_LOG = false;
constexpr bool ENABLE_VERY_VERBOSE_LOG = false;

constexpr bool DRAW_BOUNDARIES = false;

#define CARTO_LOG(format, ...) UE_LOG(LogCartograph, Display, TEXT("(%u)") TEXT(format), __LINE__ __VA_OPT__(, __VA_ARGS__))
#define CARTO_LOG_WARNING(format, ...) UE_LOG(LogCartograph, Warning, TEXT("(%u)") TEXT(format), __LINE__ __VA_OPT__(, __VA_ARGS__))
#define CARTO_LOG_ERROR(format, ...) UE_LOG(LogCartograph, Error, TEXT("(%u)") TEXT(format), __LINE__ __VA_OPT__(, __VA_ARGS__))

#define CARTO_LOG_DEBUG(format, ...) if constexpr (ENABLE_DEBUG_LOG) UE_LOG(LogCartograph, Display, TEXT(format) __VA_OPT__(, __VA_ARGS__))
#define CARTO_LOG_VERBOSE(format, ...) if constexpr (ENABLE_VERBOSE_LOG) UE_LOG(LogCartograph, Display, TEXT(format) __VA_OPT__(, __VA_ARGS__))
#define CARTO_LOG_VERY_VERBOSE(format, ...) if constexpr (ENABLE_VERY_VERBOSE_LOG) UE_LOG(LogCartograph, Display, TEXT(format) __VA_OPT__(, __VA_ARGS__))

#define CARTO_LOG_ERROR_DO_IF_NULL(ptr, action) if (!ptr) { CARTO_LOG_ERROR("'%s' is null", TEXT(#ptr)); action; }
#define CARTO_LOG_ERROR_RETURN_IF_NULL(ptr) CARTO_LOG_ERROR_DO_IF_NULL(ptr, return)
#define CARTO_LOG_ERROR_BREAK_IF_NULL(ptr) CARTO_LOG_ERROR_DO_IF_NULL(ptr, break)


// =============================================================================
// Legacy per-class config USTRUCTs. KEPT: the new FCartographClassDrawTable
// (Private/Core/CartographClassDrawTable.cpp) and the still-compiling legacy
// CartographDataStructure.cpp source these by reference as the authoritative
// per-class draw config (category colors, spline/wire stroke, layer identity).
// The re-architecture flattens them into FClassDrawInfo at gather; it does NOT
// re-author the designer-facing config schema, which BlueprintData assets bind to.
// =============================================================================
USTRUCT()
struct FCategoryData
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly)
	FLinearColor MainColor;

	UPROPERTY(EditDefaultsOnly)
	FLinearColor OutlineColor;

	UPROPERTY(EditDefaultsOnly)
	float OutlineThickness;
};


USTRUCT()
struct FSplineData
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly)
	FLinearColor Color;

	UPROPERTY(EditDefaultsOnly)
    float Thickness;
};


USTRUCT()
struct FWireData
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly)
    FLinearColor Color;

	UPROPERTY(EditDefaultsOnly)
	float Thickness;
};


USTRUCT()
struct FLayerSubCategoryData
{
	GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly)
    FName Name;

	UPROPERTY(EditDefaultsOnly)
	FText DisplayName;

	/** Lower = Earlier in the list **/
	UPROPERTY(EditDefaultsOnly)
	int Priority;
};


USTRUCT()
struct FLayerCategoryData : public FLayerSubCategoryData
{
    GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly)
	TArray<FLayerSubCategoryData> SubCategories;
};


USTRUCT()
struct FBuildLayerData
{
	GENERATED_BODY()

    UPROPERTY(EditDefaultsOnly, meta = (GetOptions = "GetLayerCategoryOptions"))
	FString Category;

	FName MainCategoryCache;
    FName SubCategoryCache;
};


// RecipeManager::Get doesn't work for clients
USTRUCT()
struct FBuildingDescriptorData
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly)
    TSubclassOf<UFGCategory> Category;

	UPROPERTY(EditDefaultsOnly)
    TSubclassOf<UFGBuildSubCategory> SubCategory;

	UPROPERTY(EditDefaultsOnly)
	TObjectPtr<UTexture2D> Icon;
};


struct FRuntimeConfig
{
	TSet<FName> DisabledLayerMainCategory;
	TMap<FName, TSet<FName>> DisabledLayerSubCategory;
	TSet<uint32> DisabledLayerBuildable;
};


/**
 * Re-architected game-instance module (SPEC STAGE2).
 *
 * OWNS the slim data spine + render/net subsystems:
 *   FCartographBuildingStore   - SoA store keyed by FBuildingHandle (no second copy)
 *   FCartographSpatialGrid     - uniform hash grid keyed by handle (no quadtree+redirector)
 *   FCartographZBandIndex      - decoupled Z-band filter (the store never sorts)
 *   FCartographClassDrawTable  - per-class draw table + shared ComputeDrawGeometry
 *   FCartographTileManager     - monotonic dirty-tile set + per-tile versions
 *   FCartographCompositor      - never-cancelled convergent tiled FCanvas (TDR fix)
 *   FCartographMapReplicator   - server-side per-tile snapshot authority (Phase-0 net)
 *
 * Build hooks do ONLY O(1) work (store mutate + grid/zband insert/erase + tile
 * MarkDirtyForBox + server version bump). The legacy O(N) redirector / O(N^2)
 * gather / cancel-restart coroutine / 256 MB monolith are all deleted.
 */
UCLASS(PrioritizeCategories=("Draw Data", "Layer Data", "UI", "Advanced", "Default", "Generated Data"))
class CARTOGRAPH_API UCartographGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

    friend class ACartographModSubsystem;
	friend class FCartographCanvasRenderItem;
	friend class UCartographRemoteCallObject;
	friend class FCartographClassDrawTable;

public:
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

	void OnWorldLoaded(UWorld* World);
	void OnWorldUnloaded();

	void OnLayerConfigChanged();

	const FBuildLayerData* GetBuildLayerData(uint32 ClassHash);

	bool DoesBuildingExist(uint32 ClassHash) const;

	template<typename T>
	const T* GetDataByBuildableClass(const TMap<TSoftClassPtr<AFGBuildable>, T>& ClassMap, const TMap<TSoftClassPtr<UFGBuildCategory>, T>& CategoryMap, UClass* BuildableClass) const;

private:
	// ---- Spine lifecycle (replaces the legacy data path entirely) ----------

	/** Allocate the spine subsystems + create the persistent atlas + start the
	 *  never-cancelled compositor. Called from OnWorldLoaded. Idempotent. */
	void InitializeSpine(UWorld* World);

	/** Release the spine (stop the compositor, clear indices). On world unload. */
	void ShutdownSpine();

	/** O(N) streaming bucketing gather over the engine subsystems by const-ref
	 *  (NO by-value engine-map copy, NO O(N^2), NO sort). Replaces the legacy
	 *  InitialBuildableGather. Interruptible under the init frame budget. */
	UE5Coro::TCoroutine<> StreamingGather(FForceLatentCoroutine = {});

	/** Drive the server-side per-tile repack + delta push once per net-tick.
	 *  No-op on listen host / single-player (no network path). */
	void ServerNetTick();

	// ---- O(1) build-hook helpers (the hot path) ----------------------------

	/** Translate a buildable class + transform + (optional) typed extra into a
	 *  slim FBuildingRecord, insert it into the store + grid + Z-band index, mark
	 *  the touched tiles dirty, and (server) bump their versions. O(1) amortized.
	 *  Returns the new stable handle (INDEX_NONE handle if not drawable/ignored). */
	FBuildingHandle AddRecordFromTransform(const TSubclassOf<AFGBuildable>& BuildableClass, const FTransform& Transform, const struct FFGDynamicStruct* TypeSpecificData, AFGBuildable* LiveBuildable);

	/** Erase a handle from the store + grid + Z-band index, mark its tiles dirty,
	 *  (server) bump versions. O(1). */
	void RemoveRecord(FBuildingHandle Handle);

	/** Find the live handle that matches a buildable identity key (for removes).
	 *  Keyed by a generation-guarded {ClassId, world-position} probe via the grid;
	 *  returns an invalid handle if not found. LiveBuildable (when available) lets
	 *  splines/wires derive the SAME component position used at insert so the probe
	 *  matches the stored Pos. */
	FBuildingHandle FindHandleForRemoval(const TSubclassOf<AFGBuildable>& BuildableClass, const FTransform& Transform, AFGBuildable* LiveBuildable = nullptr) const;

	/** Common store+grid+zband+tile insert for a built FBuildingRecord (+ optional
	 *  side-table extras already added). Used by both the hooks and the gather. */
	FBuildingHandle InsertRecord(const FBuildingRecord& Record);

	/** Common store+grid+zband+tile erase for a live handle. */
	void EraseRecord(FBuildingHandle Handle);

	void RegisterMenuButton() const;

	void LoadRuntimeConfig();
    void SaveRuntimeConfig();

	void FillBuildLayerDataCache();

	void GatherBuildables();
	void GatherModOverrides();

	static TSet<FTopLevelAssetPath> GetDerivedClassPaths(UClass* ParentClass);

	// For blueprint use only
private:
	UFUNCTION(BlueprintCallable)
	void OnZFilterUpdated(float Min, float Max);

	UFUNCTION(BlueprintCallable)
	void OnCartographMenuButtonClicked(UUserWidget* Widget, bool IsOpen);

	UFUNCTION(BlueprintCallable)
	void OnShowBuildingsCheckboxChanged(bool DoShow);

	UFUNCTION()
	TArray<FString> GetLayerCategoryOptions() const;

	// Bound BY NAME from the SML Hook_MapMenu_Cartograph blueprint asset (the vanilla
	// map-open hook). MUST exist for that .uasset to cook even though the hook does not
	// fire at runtime on the current SDK. Does only UI bookkeeping (no compositor); the
	// render is data-driven + debounced, not gated on map-open.
	UFUNCTION(BlueprintCallable)
	void OnVanillaMapMenuShown(const UUserWidget* Widget) const;


	template<typename T>
	void FillInMatchingProperties(const FProperty* StructPropertyToCompare, TArray<std::pair<const FProperty*, const FProperty*>>& Out);

	template<typename KeyType, typename ValueType>
	void ProcessOverrideData(TMap<KeyType, ValueType>& MapToBeOverriden, UClass* OverrideDataClass, FName PropertyName);

	template<typename T>
	void ProcessOverrideData(TSet<T>& MapToBeOverriden, UClass* OverrideDataClass, FName PropertyName);

	void ProcessLayerCategoriesOverride(UClass* OverrideDataClass);


public:
	inline static UCartographGameInstanceModule* Instance = nullptr;

	/** Drive the top-right map status text (reuses the IsInitializing/InitializeProgress
	 *  UPROPERTYs the UMG already binds as "Initializing..(N%)"). The compositor calls this
	 *  each tick to surface map-build convergence: bActive shows/hides the text, Progress
	 *  (0..1) fills the percent. Display-only; IsInitializing has no control-flow readers. */
	void SetMapBuildStatus(bool bActive, float Progress)
	{
		IsInitializing = bActive;
		InitializeProgress = Progress;
	}

	FRuntimeConfig RuntimeConfig;

#pragma region Static Data
	UPROPERTY()  // Generated Data
    TMap<uint32, TSubclassOf<AFGBuildable>> ClassIDToClassPtrMap;
	UPROPERTY()  // Generated Data
    TMap<TSubclassOf<AFGBuildable>, uint32> ClassPtrToClassIDMap;

	UPROPERTY()  // Generated Data
	TMap<TSubclassOf<AFGBuildable>, FBuildingDescriptorData> ClassPtrToDescriptorDataMap;

	TMap<TSoftClassPtr<AFGBuildable>, FString> ModdedBuildings;
    TMap<FString, FBuildLayerData> ModdedBuildLayerData;


	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Category Data")
	TMap<TSoftClassPtr<UFGBuildCategory>, FCategoryData> BuildCategoryDataMap;

	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Category Data")
	TMap<TSoftClassPtr<AFGBuildable>, FCategoryData> BuildableBuildCategoryDataOverrideMap;

	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Category Data")
	TMap<TSubclassOf<UFGFactoryCustomizationDescriptor_Material>, FCategoryData> MaterialBuildCategoryDataOverrideMap;


	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Override")
	TMap<TSoftClassPtr<AFGBuildable>, TSoftObjectPtr<UTexture2D>> BuildableIconOverrideMap;

	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Override")
	TMap<TSoftClassPtr<AFGBuildable>, FVector2D> BuildableSizeOverrideMap;

	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Override")
	TMap<TSoftClassPtr<AFGBuildable>, FRotator> BuildableExtraRotationMap;


	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Special Data")
	TMap<TSoftClassPtr<AFGBuildable>, FSplineData> BuildableSplineDataMap;

	UPROPERTY(EditDefaultsOnly, Category = "Draw Data/Special Data")
    TMap<TSoftClassPtr<AFGBuildable>, FWireData> BuildableWireDataMap;


	UPROPERTY(EditDefaultsOnly, Category = "Default")
	TMap<TSoftClassPtr<AFGBuildable>, TSoftClassPtr<AFGBuildable>> BuildableClassRedirectMap;

    UPROPERTY(EditDefaultsOnly, Category = "Default")
	TSet<TSoftClassPtr<AFGBuildable>> BuildableToIgnore;


	UPROPERTY(EditDefaultsOnly, Category = "Layer Data")
	TArray<FLayerCategoryData> LayerCategories;

	UPROPERTY(EditDefaultsOnly, Category = "Layer Data")
	TMap<TSoftClassPtr<UFGBuildCategory>, FBuildLayerData> BuildLayerDataMap;

	UPROPERTY(EditDefaultsOnly, Category = "Layer Data")
	TMap<TSoftClassPtr<AFGBuildable>, FBuildLayerData> BuildableBuildLayerDataOverrideMap;

	UPROPERTY(EditDefaultsOnly, Category = "Layer Data")
	TMap<TSubclassOf<UFGFactoryCustomizationDescriptor_Material>, FBuildLayerData> MaterialBuildLayerDataOverrideMap;


	static constexpr const char* UnspecifiedMainCategory = "Modded";

	UPROPERTY(EditDefaultsOnly, Category = "Unspecified Default Data")
	FCategoryData UnspecifiedCategoryData;

	// SegmentsConfigName should be None.
	UPROPERTY(EditDefaultsOnly, Category = "Unspecified Default Data")
	FSplineData UnspecifiedSplineData;

	UPROPERTY(EditDefaultsOnly, Category = "Unspecified Default Data")
    int UnspecifiedSplineSegments;

    UPROPERTY(EditDefaultsOnly, Category = "Unspecified Default Data")
    FWireData UnspecifiedWireData;

protected:
	UPROPERTY(EditDefaultsOnly, Category = "UI")
	TObjectPtr<UCanvasRenderTarget2D> RenderTarget;

	UPROPERTY(EditDefaultsOnly, Category = "UI")
	TSoftClassPtr<UUserWidget> MapContainerWidget;

	UPROPERTY(EditDefaultsOnly, Category = "UI")
	TSubclassOf<UUserWidget> MenuShowHideButtonWidget;

	UPROPERTY(EditDefaultsOnly, Category = "UI")
	TSubclassOf<UUserWidget> MenuWidget;
#pragma endregion


	TMap<uint32, const FBuildLayerData*> BuildLayerDataMapCache;

	/** Live building count per class hash. KEPT: the UI's DoesBuildingExist reads
	 *  it (CartographMenuLayerItemWidget). Maintained O(1) in the build hooks. */
	TMap<uint32, uint32> BuildingCountMap;

	/** ClassId -> class hash reverse lookup so RemoveRecord can decrement
	 *  BuildingCountMap in O(1) (instead of scanning all classes). Built once in
	 *  InitializeSpine after ClassDrawTable.Build. Index is the dense ClassId. */
	TArray<uint32> ClassIdToHash;


	bool ShouldInitialize = false;
	UPROPERTY(BlueprintReadOnly)
	bool IsInitializing = false;

	// ---- The slim spine (replaces CurrentBuildingData + quadtree + redirector) -
	// Plain C++ members (no UPROPERTY): they hold no UObject refs the GC must
	// trace except the atlas (a separate rooted UPROPERTY) and the pinned icons
	// (rooted inside the compositor). The store/grid/zband are pure POD indices.
	FCartographBuildingStore BuildingStore;
	FCartographSpatialGrid SpatialGrid;
	FCartographZBandIndex ZBandIndex;
	FCartographClassDrawTable ClassDrawTable;
	FCartographTileManager TileManager;
	FCartographCompositor Compositor;
	FCartographMapReplicator MapReplicator;

	/** The never-cancelled compositor drain loop. Started ONCE in InitializeSpine,
	 *  completes only on ShutdownSpine (it is never Cancel()'d). */
	UE5Coro::TCoroutine<> CompositorCoroutine = UE5Coro::TCoroutine<>::CompletedCoroutine;

	/** The interruptible O(N) initial gather (replaces InitialBuildableGather). */
	UE5Coro::TCoroutine<> GatherCoroutine = UE5Coro::TCoroutine<>::CompletedCoroutine;

	bool bSpineInitialized = false;
	bool IsInWorld = false;
    bool IsClient = false;          // dedicated client (NM_Client)
	bool bIsDedicatedServer = false;

	// For blueprint use only
protected:
	UPROPERTY(BlueprintReadOnly)
	float InitializeProgress = 0;

	UPROPERTY(BlueprintReadOnly)
	float MinHeight = -100;
	UPROPERTY(BlueprintReadOnly)
	float MaxHeight = 100;

	float MinCached = 0.f;
    float MaxCached = 1.f;

    UPROPERTY(BlueprintReadOnly)
    bool DoShowBuildings = true;
};



template<typename T>
const T* UCartographGameInstanceModule::GetDataByBuildableClass(const TMap<TSoftClassPtr<AFGBuildable>, T>& ClassMap, const TMap<TSoftClassPtr<UFGBuildCategory>, T>& CategoryMap, UClass* BuildableClass) const
{
	const T* Data = ClassMap.Find(BuildableClass);
	if (!Data)
	{
		const FBuildingDescriptorData* DescriptorData = ClassPtrToDescriptorDataMap.Find(BuildableClass);
		if (!DescriptorData)
		{
			CARTO_LOG_WARNING("Can't find descriptor data for %s", *BuildableClass->GetName());
			return nullptr;
		}

		Data = CategoryMap.Find(DescriptorData->SubCategory.Get());
		if (!Data)
		{
			Data = CategoryMap.Find(DescriptorData->Category.Get());
		}
	}

	return Data;
}


template<typename T>
concept IsFVector = std::is_same_v<T, FVector> || std::is_same_v<T, FVector2D>;


// Legacy world<->screen helpers. KEPT: the still-compiling legacy
// CartographDataStructure.cpp uses these (FillInCache draw-math path). They now
// resolve RENDER_TEXTURE_SIZE / ORIGIN_UV / MAP_*_CENTIMETERS from CartographConfig.h
// (4096), self-consistent with the new atlas. New code uses CartographCoords::*.
template<IsFVector T, IsFVector U>
FVector2D world_position_to_screen_position(const T& WorldPosition, const U& Size)
{
	return FVector2D{
		ORIGIN_UV[0] + (WorldPosition.X - Size.X / 2) / MAP_WIDTH_CENTIMETERS,
		ORIGIN_UV[1] + (WorldPosition.Y - Size.Y / 2) / MAP_HEIGHT_CENTIMETERS
	} * RENDER_TEXTURE_SIZE;
}


template<IsFVector T>
FVector2D screen_position_to_world_position(const T& ScreenPosition)
{
    return FVector2D{
        (ScreenPosition.X / RENDER_TEXTURE_SIZE - ORIGIN_UV[0]) * MAP_WIDTH_CENTIMETERS,
        (ScreenPosition.Y / RENDER_TEXTURE_SIZE - ORIGIN_UV[1]) * MAP_HEIGHT_CENTIMETERS
    };
}
