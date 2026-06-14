#include "Core/CartographClassDrawTable.h"

#include "Core/CartographBuildingStore.h"

#include "Engine/InheritableComponentHandler.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/SceneComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Texture2D.h"

// The legacy CARTO_LOG / LogCartograph macros and the legacy config TMaps both
// live on the game-instance module header. We include it here (private .cpp,
// not the contract header) so the parameterized Build can read the authoritative
// per-class config and so we reuse the exact CARTO_LOG_* macros (repo style).
#include "CartographGameInstanceModule.h"
#include "CartographDataStructure.h"

// FactoryGame buildable taxonomy - the legacy class-classification cascade keys
// off these exact interfaces / base classes (CartographDataStructure.cpp:289-371).
#include "Buildables/FGBuildable.h"
#include "FGBuildableBeam.h"
#include "Buildables/FGBuildableWire.h"
#include "FGSplineBuildableInterface.h"


// =============================================================================
// CartographClassDrawTable.cpp
// -----------------------------------------------------------------------------
// Per-class draw table + the shared, pure ComputeDrawGeometry.
//
// ComputeDrawGeometry is a *pixel-identical* port of the legacy
// FBuildingData::FillInCache + FillInVisualBoxCache draw-math
// (CartographDataStructure.cpp:264-547). The legacy design pre-computed and
// stored that geometry per building; here it is recomputed on demand from the
// slim record + the per-class table, which is what lets the stored caches be
// deleted (SPEC 4.1, diagnosis 6).
//
// DIFF-TEST(Q12): Before deleting the legacy FBuildingData caches, a pixel
// -identity harness MUST be run (SPEC 10, Q12). Concretely:
//   1. Load a large save under the LEGACY build; for every FBuildingData, dump
//      (DataType, ScreenPosition, Size, Rotation, world Corners[4], VisualBox).
//   2. For the same save under the NEW build, construct the equivalent
//      FBuildingRecord (Pos = Transform.Location, PackedYaw = quantized
//      Transform yaw, ClassId via GetClassId, Type from the cascade below,
//      ExtraIndex into the store side-tables) and call ComputeDrawGeometry.
//   3. Project the legacy world Corners[4] to screen with
//      CartographCoords::WorldToScreen(Corner) (== legacy
//      world_position_to_screen_position(Corner, FVector::ZeroVector)) and
//      compare against OutGeometry.Corners[4]; compare ScreenPos/Size/Rotation/
//      VisualBox under a sub-pixel epsilon (the screen space is 4096, legacy was
//      8192 - normalize by RENDER_TEXTURE_SIZE before comparing, or run the diff
//      with RENDER_TEXTURE_SIZE temporarily pinned to 8192).
//   4. KNOWN, INTENTIONAL divergences the harness must tolerate / flag (they are
//      data-model decisions, NOT math bugs - see the per-site notes below):
//        (a) the slim record drops per-instance Scale3D, so scaled buildings
//            (foundations placed with a scale, beams) differ from legacy where
//            legacy did `Size *= Transform.GetScale3D()`. ComputeDrawGeometry
//            uses scale = 1.
//        (b) the slim record keeps only yaw, so the corner transform here is
//            yaw-only; legacy used the full FRotator (pitch/roll) and the full
//            extra-rotation FRotator. For top-down maps pitch/roll were already
//            visually irrelevant, but the harness should bucket any building with
//            non-trivial pitch/roll separately rather than fail the whole diff.
//   Render both atlases for the same save and visually diff; gate the cache
//   deletion on a clean diff modulo (a)/(b).
// =============================================================================


// -----------------------------------------------------------------------------
// Internal helpers.
// -----------------------------------------------------------------------------
namespace
{
	/** Resolve a (possibly redirected) buildable class through the legacy
	 *  BuildableClassRedirectMap, exactly as FillInCache/FillInHash did
	 *  (CartographDataStructure.cpp:270-272). LoadSynchronous matches legacy. */
	const UClass* ResolveRedirectedClass(UCartographGameInstanceModule* Module, const UClass* OriginalClass)
	{
		if (!Module || !OriginalClass)
		{
			return OriginalClass;
		}

		// BuildableClassRedirectMap is keyed by TSoftClassPtr<AFGBuildable>. Build the
		// key explicitly (rather than relying on an implicit UClass* -> TSoftClassPtr
		// ctor inside Find, which some UE versions mark explicit) so the lookup hashes
		// and compares over the soft object path, which is the intended key identity.
		const TSoftClassPtr<AFGBuildable>* RedirectClass =
			Module->BuildableClassRedirectMap.Find(TSoftClassPtr<AFGBuildable>(const_cast<UClass*>(OriginalClass)));
		if (RedirectClass)
		{
			if (UClass* Loaded = RedirectClass->LoadSynchronous())  // SPIKE(Q1): TSoftClassPtr::LoadSynchronous available on CSS fork (used by legacy, so safe)
			{
				return Loaded;
			}
		}
		return OriginalClass;
	}
}


// -----------------------------------------------------------------------------
// Footprint derivation. Pixel-identical port of the legacy GetBuildingSize
// cascade (CartographDataStructure.cpp:565-655); SPEC 8 salvage of
// Cartograph_2.0 GetBuildingBoundingBox. Single-precision FVector2f out.
// -----------------------------------------------------------------------------
FVector2f FCartographClassDrawTable::DeriveFootprintFromCDO(const UClass* BuildableClass)
{
	if (!BuildableClass)
	{
		return FVector2f::ZeroVector;
	}

	// Step 2: clearance box (the primary, cheap path the engine exposes per CDO).
	// SPIKE(Q2): AFGBuildable::GetCombinedClearanceBox() reachable from a mod on
	// the CSS fork - the legacy code calls it directly, so it is mod-public here.
	const AFGBuildable* CDO = GetDefault<AFGBuildable>(const_cast<UClass*>(BuildableClass));
	if (!CDO)
	{
		CARTO_LOG_ERROR("Can't find CDO for %s", *BuildableClass->GetName());
		return FVector2f::ZeroVector;
	}

	if (const FBox ClearanceBox = CDO->GetCombinedClearanceBox();
		ClearanceBox.IsValid)
	{
		return FVector2f(FVector2D{ ClearanceBox.GetSize() });
	}

	// Step 3: CDO-walk fallback. Union the local bounds of every SceneComponent
	// reachable through the BlueprintGeneratedClass ICH + SCS chain (up the
	// super-class chain) and the native CDO default subobjects.
	FBox Box(ForceInit);

	const UBlueprintGeneratedClass* BlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(const_cast<UClass*>(BuildableClass));
	while (BlueprintGeneratedClass)
	{
		if (const UInheritableComponentHandler* InheritableHandler = BlueprintGeneratedClass->InheritableComponentHandler)
		{
			TArray<UActorComponent*> Templates;
			InheritableHandler->GetAllTemplates(Templates, true);
			for (const UActorComponent* ComponentTemplate : Templates)
			{
				if (const USceneComponent* Component = Cast<USceneComponent>(ComponentTemplate))
				{
					Box += Component->GetLocalBounds().GetBox();
				}
			}
		}

		if (const USimpleConstructionScript* ConstructionScript = BlueprintGeneratedClass->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : ConstructionScript->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}

				if (const USceneComponent* Component = Cast<USceneComponent>(Node->ComponentTemplate))
				{
					Box += Component->GetLocalBounds().GetBox();
				}
			}
		}

		BlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(BlueprintGeneratedClass->GetSuperClass());
	}

	UClass* NativeClass = const_cast<UClass*>(BuildableClass);
	while (NativeClass)
	{
		TArray<UObject*> DefaultObjectSubobjects;
		NativeClass->GetDefaultObjectSubobjects(DefaultObjectSubobjects);

		for (const UObject* DefaultSubObject : DefaultObjectSubobjects)
		{
			if (const USceneComponent* Component = Cast<USceneComponent>(DefaultSubObject))
			{
				Box += Component->GetLocalBounds().GetBox();
			}
		}

		NativeClass = NativeClass->GetSuperClass();
	}

	if (!Box.GetSize().IsZero())
	{
		CARTO_LOG_DEBUG("Calculated building footprint from CDO: %s", *BuildableClass->GetName());
		return FVector2f(FVector2D{ Box.GetSize() });
	}

	CARTO_LOG_WARNING("Can't find footprint for %s", *BuildableClass->GetName());
	return FVector2f::ZeroVector;
}


// -----------------------------------------------------------------------------
// Build (no-arg). The frozen contract form. Without the legacy config source it
// can only produce an empty table; the real population happens in the
// parameterized overload that receives the game-instance module.
// -----------------------------------------------------------------------------
void FCartographClassDrawTable::Build()
{
	Build(UCartographGameInstanceModule::Instance);
}


// -----------------------------------------------------------------------------
// Build (parameterized). Mirrors the legacy FillInCache class-classification
// cascade once per distinct class, instead of once per building.
//
// For each gathered buildable class (Module->ClassPtrToClassIDMap keys), resolve
// its redirect, classify it (Spline / Wire / Beam / Icon / Rectangle) exactly as
// FillInCache did, and fill an FClassDrawInfo. Unclassifiable / undrawable
// classes still get a ClassId (so records always resolve) but with
// DrawType = Invalid, which ComputeDrawGeometry early-outs on.
// -----------------------------------------------------------------------------
void FCartographClassDrawTable::Build(UCartographGameInstanceModule* Module)
{
	ClassToId.Empty();
	Infos.Empty();

	if (!Module)
	{
		CARTO_LOG_ERROR("ClassDrawTable::Build called with null module");
		return;
	}

	ClassToId.Reserve(Module->ClassPtrToClassIDMap.Num());
	Infos.Reserve(Module->ClassPtrToClassIDMap.Num());

	for (const auto& [OriginalSoftClass, ClassHash] : Module->ClassPtrToClassIDMap)
	{
		UClass* OriginalClass = OriginalSoftClass.Get();
		if (!OriginalClass || Module->BuildableToIgnore.Contains(OriginalClass))
		{
			continue;
		}

		// Assign a dense ClassId keyed by the ORIGINAL (pre-redirect) class, since
		// that is the class a record's ClassId will be looked up by (the build
		// hook sees the original buildable class). Skip duplicates defensively.
		if (ClassToId.Contains(OriginalClass))
		{
			continue;
		}

		const UClass* BuildableClass = ResolveRedirectedClass(Module, OriginalClass);
		if (!BuildableClass)
		{
			continue;
		}

		FClassDrawInfo Info;

		// Layer identity (filter/toggle). Legacy stored a pointer to FBuildLayerData;
		// here we flatten the two cached FNames the renderer actually keys on.
		if (const FBuildLayerData* LayerData = Module->GetBuildLayerData(ClassHash))
		{
			Info.LayerMainCategory = LayerData->MainCategoryCache;
			Info.LayerSubCategory = LayerData->SubCategoryCache;
		}

		const bool bIsModded = Module->ModdedBuildings.Contains(const_cast<UClass*>(BuildableClass));

		// ---- Classification cascade (legacy FillInCache:289-446) --------------
		if (BuildableClass->ImplementsInterface(UFGSplineBuildableInterface::StaticClass()))  // SPIKE(Q1): UFGSplineBuildableInterface mod-reachable (legacy uses it)
		{
			const FSplineData* SplineData = Module->BuildableSplineDataMap.Find(const_cast<UClass*>(BuildableClass));
			if (!SplineData && bIsModded)
			{
				SplineData = &Module->UnspecifiedSplineData;
			}
			if (SplineData && SplineData->Thickness > 0)  // legacy: Thickness <= 0 => not drawn
			{
				Info.DrawType = EBuildingDrawType::Spline;
				Info.StrokeColor = SplineData->Color;
				Info.StrokeThickness = SplineData->Thickness;
			}
			// else leave DrawType = Invalid (legacy returns without a cache)
		}
		else if (BuildableClass->IsChildOf(AFGBuildableWire::StaticClass()))
		{
			const FWireData* WireData = Module->BuildableWireDataMap.Find(const_cast<UClass*>(BuildableClass));
			if (!WireData && bIsModded)
			{
				WireData = &Module->UnspecifiedWireData;
			}
			if (WireData && WireData->Thickness > 0)
			{
				Info.DrawType = EBuildingDrawType::Wire;
				Info.StrokeColor = WireData->Color;
				Info.StrokeThickness = WireData->Thickness;
			}
		}
		else if (BuildableClass->IsChildOf(AFGBuildableBeam::StaticClass()))
		{
			// Legacy beams read from BuildableWireDataMap (NOT a separate beam map).
			const FWireData* BeamData = Module->BuildableWireDataMap.Find(const_cast<UClass*>(BuildableClass));
			if (!BeamData && bIsModded)
			{
				BeamData = &Module->UnspecifiedWireData;
			}
			if (BeamData && BeamData->Thickness > 0)
			{
				Info.DrawType = EBuildingDrawType::Beam;
				Info.StrokeColor = BeamData->Color;
				Info.StrokeThickness = BeamData->Thickness;
			}
		}
		else
		{
			// Normal (Icon or Rectangle). Footprint first - zero footprint is not
			// drawn (legacy FillInCache:373-377).
			const FVector2f Footprint = [&]() -> FVector2f
			{
				// Legacy GetBuildingSize: size-override map first, then CDO derivation.
				if (const FVector2D* Override = Module->BuildableSizeOverrideMap.Find(const_cast<UClass*>(BuildableClass)))
				{
					return FVector2f(*Override);
				}
				return DeriveFootprintFromCDO(BuildableClass);
			}();

			if (Footprint.X != 0.f && Footprint.Y != 0.f)
			{
				Info.Footprint = Footprint;

				// Extra rotation (yaw only for top-down - SPEC 4.1; legacy added the
				// full FRotator but only Yaw survives the slim record - see Q12 (b)).
				if (const FRotator* ExtraRotation = Module->BuildableExtraRotationMap.Find(const_cast<UClass*>(BuildableClass)))
				{
					Info.ExtraYawDegrees = (float)ExtraRotation->Yaw;
				}

				// Icon override wins over category fill (legacy FillInCache:389-441).
				const TSoftObjectPtr<UTexture2D>* IconOverride = Module->BuildableIconOverrideMap.Find(const_cast<UClass*>(BuildableClass));
				if (IconOverride && !IconOverride->IsNull())
				{
					Info.DrawType = EBuildingDrawType::Icon;
					Info.Icon = *IconOverride;
				}
				else
				{
					const FCategoryData* CategoryData = Module->GetDataByBuildableClass(
						Module->BuildableBuildCategoryDataOverrideMap,
						Module->BuildCategoryDataMap,
						const_cast<UClass*>(BuildableClass));
					if (!CategoryData && bIsModded)
					{
						CategoryData = &Module->UnspecifiedCategoryData;
					}
					if (CategoryData)
					{
						Info.DrawType = EBuildingDrawType::Rectangle;
						Info.MainColor = CategoryData->MainColor;
						Info.OutlineColor = CategoryData->OutlineColor;
						Info.OutlineThickness = CategoryData->OutlineThickness;
					}
					// else DrawType stays Invalid (legacy warns "Can't find category data")
				}
			}
		}

		const uint16 NewId = (uint16)Infos.Num();
		Infos.Add(MoveTemp(Info));
		ClassToId.Add(OriginalClass, NewId);
	}

	CARTO_LOG("ClassDrawTable built. Distinct classes: %d", Infos.Num());
}


// -----------------------------------------------------------------------------
// Lookup.
// -----------------------------------------------------------------------------
uint16 FCartographClassDrawTable::GetClassId(const UClass* BuildableClass) const
{
	if (const uint16* Found = ClassToId.Find(BuildableClass))
	{
		return *Found;
	}
	return InvalidClassId;
}


const FClassDrawInfo& FCartographClassDrawTable::GetInfo(uint16 ClassId) const
{
	// PRECONDITION: ClassId valid. checkSlow keeps the hot path branch-free in
	// shipping while catching contract violations in dev.
	checkSlow(ClassId != InvalidClassId && (int32)ClassId < Infos.Num());
	return Infos[ClassId];
}


const FClassDrawInfo* FCartographClassDrawTable::FindInfo(uint16 ClassId) const
{
	if (ClassId == InvalidClassId || (int32)ClassId >= Infos.Num())
	{
		return nullptr;
	}
	return &Infos[ClassId];
}


int32 FCartographClassDrawTable::NumClasses() const
{
	return Infos.Num();
}


void FCartographClassDrawTable::GatherIconAssets(TArray<TSoftObjectPtr<UTexture2D>>& OutIcons) const
{
	for (const FClassDrawInfo& Info : Infos)
	{
		if (Info.DrawType == EBuildingDrawType::Icon && !Info.Icon.IsNull())
		{
			OutIcons.AddUnique(Info.Icon);
		}
	}
}


// =============================================================================
// ComputeDrawGeometry - the pixel-identical port.
// =============================================================================
namespace
{
	// Project a world XY point to screen, matching legacy
	// world_position_to_screen_position(Point, FVector::ZeroVector) used by
	// draw_line at draw time (no footprint centering offset on stroke endpoints).
	template<typename TPos>
	FORCEINLINE FVector2f ProjectPoint(const TPos& WorldXY)
	{
		return CartographCoords::WorldToScreen(WorldXY);
	}

	// Build the legacy local-space corner quad for a footprint Size (cm). Order
	// matches CartographDataStructure.cpp:423-426 / 529-532 exactly:
	//   [0] (-W,-H) [1] (+W,-H) [2] (+W,+H) [3] (-W,+H), CW from top-left.
	FORCEINLINE void MakeLocalCorners(const FVector2D& Size, FVector(&OutCorners)[4])
	{
		const double HalfWidth = Size.X / 2.0;
		const double HalfHeight = Size.Y / 2.0;
		OutCorners[0] = FVector(-HalfWidth, -HalfHeight, 0.0);
		OutCorners[1] = FVector(HalfWidth, -HalfHeight, 0.0);
		OutCorners[2] = FVector(HalfWidth, HalfHeight, 0.0);
		OutCorners[3] = FVector(-HalfWidth, HalfHeight, 0.0);
	}
}


bool FCartographClassDrawTable::ComputeDrawGeometry(
	const FBuildingRecord& Record,
	const FClassDrawInfo& Info,
	const FCartographBuildingStore& Store,
	FDrawGeometry& OutGeometry)
{
	OutGeometry = FDrawGeometry{};
	OutGeometry.Type = Info.DrawType;

	// VisualBox accumulates in WORLD cm (exactly like the legacy
	// FillInVisualBoxCache), is expanded by BOX_EXPANSION_CENTIMETERS in world cm,
	// then projected to screen at the end - this reproduces the legacy expand-in
	// -world-then-the-renderer-reads-world-box behaviour. Keeping the expansion in
	// world space (not an ad-hoc screen epsilon) is what makes the dirty-rect
	// bookkeeping pixel-faithful (SPEC FDrawGeometry::VisualBox note, Q12).
	FBox2D WorldBox(ForceInit);

	const FVector3f Pos3 = Record.Pos;
	const FVector2D WorldPosXY{ (double)Pos3.X, (double)Pos3.Y };

	switch (Info.DrawType)
	{
	case EBuildingDrawType::Spline:
	{
		if (Info.StrokeThickness <= 0.f || Record.ExtraIndex == (uint32)INDEX_NONE)
		{
			return false;  // legacy: thickness <= 0 => not drawn / no extra => can't draw
		}

		const FSplineExtra& Extra = Store.GetSplineExtra(Record.ExtraIndex);
		const int32 Num = Extra.Points.Num();
		if (Num < 2)
		{
			return false;  // a single point cannot stroke a polyline
		}

		OutGeometry.SplinePointsScreen.Reserve(Num);
		for (const FVector2f& WorldPt : Extra.Points)
		{
			OutGeometry.SplinePointsScreen.Add(ProjectPoint(WorldPt));
			WorldBox += FVector2D{ (double)WorldPt.X, (double)WorldPt.Y };  // legacy FillInSplineVisualBoxCache
		}

		OutGeometry.StrokeColor = Info.StrokeColor;
		OutGeometry.StrokeThickness = Info.StrokeThickness;
		break;
	}

	case EBuildingDrawType::Wire:
	{
		if (Info.StrokeThickness <= 0.f || Record.ExtraIndex == (uint32)INDEX_NONE)
		{
			return false;
		}

		const FWireExtra& Extra = Store.GetWireExtra(Record.ExtraIndex);
		const FVector2D EndXY{ (double)Extra.End.X, (double)Extra.End.Y };

		// Legacy: start = Transform.GetLocation().XY, end = WireExtraData->End.
		OutGeometry.ScreenPos = ProjectPoint(WorldPosXY);
		OutGeometry.WireOrBeamEndScreen = ProjectPoint(EndXY);
		OutGeometry.StrokeColor = Info.StrokeColor;
		OutGeometry.StrokeThickness = Info.StrokeThickness;

		WorldBox += WorldPosXY;  // legacy FillInVisualBoxCache wire branch
		WorldBox += EndXY;
		break;
	}

	case EBuildingDrawType::Beam:
	{
		if (Info.StrokeThickness <= 0.f || Record.ExtraIndex == (uint32)INDEX_NONE)
		{
			return false;
		}

		const FBeamExtra& Extra = Store.GetBeamExtra(Record.ExtraIndex);
		const float Length = Extra.Length;

		// Legacy: end = Start + Transform.GetRotation().Vector() * Length, where
		// Vector() is the forward (+X) axis of the rotation. With only yaw on the
		// slim record, forward = (cos yaw, sin yaw, 0). DIFF-TEST(Q12)(b): legacy
		// used the full quat forward (pitch/roll tilt the beam); top-down only the
		// XY projection of that forward matters, and for a yaw-only beam it is
		// identical.
		const double YawRad = FMath::DegreesToRadians((double)Record.GetYawDegrees());
		const FVector2D ForwardXY{ FMath::Cos(YawRad), FMath::Sin(YawRad) };
		const FVector2D EndXY = WorldPosXY + ForwardXY * (double)Length;

		OutGeometry.ScreenPos = ProjectPoint(WorldPosXY);
		OutGeometry.WireOrBeamEndScreen = ProjectPoint(EndXY);
		OutGeometry.StrokeColor = Info.StrokeColor;
		OutGeometry.StrokeThickness = Info.StrokeThickness;

		WorldBox += WorldPosXY;  // legacy FillInVisualBoxCache beam branch
		WorldBox += EndXY;
		break;
	}

	case EBuildingDrawType::Icon:
	case EBuildingDrawType::Rectangle:
	{
		// Footprint must be non-zero (legacy FillInCache:373-377 early-out).
		if (Info.Footprint.X == 0.f || Info.Footprint.Y == 0.f)
		{
			return false;
		}

		// DIFF-TEST(Q12)(a): legacy did `Size *= Transform.GetScale3D()`. The slim
		// record drops scale, so scale = 1 here. Footprint is the unscaled cm size.
		const FVector2D Size{ (double)Info.Footprint.X, (double)Info.Footprint.Y };

		// Final yaw = transform yaw + class ExtraYaw (legacy added the full extra
		// FRotator; only the Yaw component is meaningful top-down - Q12 (b)).
		const float FinalYaw = Record.GetYawDegrees() + Info.ExtraYawDegrees;
		OutGeometry.RotationDeg = FinalYaw;

		// Anchor: legacy world_position_to_screen_position(Location, Size) -
		// centres the footprint (the draw then sets PivotPoint {0.5,0.5}). The
		// no-Size vs Size overload distinction is load-bearing for pixel identity:
		// the tile anchor MUST use the Size overload (CartographCoords::WorldToScreen
		// with Size), exactly the legacy call.
		OutGeometry.ScreenPos = CartographCoords::WorldToScreen(WorldPosXY, Size);

		// FDrawGeometry.Size is documented as PIXELS (CartographTypes.h:216-217) and
		// every consumer (compositor DrawGeometry, Slate AppendInstance) passes it
		// straight through as pixels. Legacy converted the cm footprint to px per axis
		// via PIXEL_PER_CENTIMETER (anisotropic: X and Y use different scales) before
		// handing it to the FCanvasTileItem. Reproduce that conversion here so both
		// render backends receive pixel sizes, not raw cm (PIXEL_PER_CENTIMETER
		// ~= 0.0055, so skipping it would render icons/rects ~180x oversized).
		OutGeometry.Size = FVector2f(
			(float)(Size.X * PIXEL_PER_CENTIMETER[0]),
			(float)(Size.Y * PIXEL_PER_CENTIMETER[1]));

		if (Info.DrawType == EBuildingDrawType::Icon)
		{
			OutGeometry.Icon = Info.Icon;
		}
		else  // Rectangle: fill + outline color/thickness.
		{
			OutGeometry.MainColor = Info.MainColor;
			OutGeometry.OutlineColor = Info.OutlineColor;
			OutGeometry.OutlineThickness = Info.OutlineThickness;
		}

		// Corners. Legacy built a +/- half-extent quad (scaled), transformed it by
		// the no-scale transform into WORLD space (CartographDataStructure.cpp
		// :421-433), and the renderer projected each to screen at draw via
		// world_position_to_screen_position(Corner, ZeroVector). We reproduce:
		//   1. yaw-only transform (Q12 (b): no pitch/roll on the slim record),
		//   2. scale = 1 (Q12 (a)),
		//   3. project each world corner to screen so OutGeometry.Corners[4] is
		//      already screen-space (the contract wants screen-space corners).
		// The corner rotation uses ONLY the record yaw - NOT FinalYaw - because
		// legacy applied the extra rotation solely to the tile-item Rotation, never
		// to the corner transform (FillInCache:417-433 rotates corners by the bare
		// `Transform`; the ExtraRotation went only into the `Rotation` field at
		// :380-385). This split is load-bearing for pixel identity.
		//
		// Both Icon and Rectangle compute the corner union: legacy
		// FillInVisualBoxCache's "normal" branch (CartographDataStructure.cpp
		// :517-541) re-derived the corner quad for BOTH icon and rectangle records,
		// so an icon's VisualBox is the corner union too - only the screen-space
		// Corners[4] output differs (meaningful for Rectangle, left zero for Icon).
		FVector LocalCorners[4];
		MakeLocalCorners(Size, LocalCorners);

		const FRotator YawOnly(0.0, (double)Record.GetYawDegrees(), 0.0);
		const FTransform CornerTransform(YawOnly, FVector(WorldPosXY.X, WorldPosXY.Y, (double)Pos3.Z), FVector::OneVector);

		for (int32 i = 0; i < 4; ++i)
		{
			const FVector WorldCorner = CornerTransform.TransformPosition(LocalCorners[i]);
			const FVector2D WorldCornerXY{ WorldCorner.X, WorldCorner.Y };
			WorldBox += WorldCornerXY;  // legacy FillInVisualBoxCache normal branch
			if (Info.DrawType == EBuildingDrawType::Rectangle)
			{
				OutGeometry.Corners[i] = ProjectPoint(WorldCornerXY);
			}
		}
		break;
	}

	case EBuildingDrawType::Invalid:
	default:
		return false;  // legacy: Invalid data type is never drawn
	}

	// ---- VisualBox: expand in world cm, then project to screen ----------------
	// Legacy: VisualBoxCache.ExpandBy(BoxExpansionCentimeters) in WORLD cm, only
	// when the box is valid (CartographDataStructure.cpp:543-546, 561). We expand
	// in world cm then project the two extreme corners to screen so the screen
	// -space VisualBox the new dirty-rect machinery consumes is the faithful
	// projection of the legacy world box.
	if (WorldBox.bIsValid)
	{
		WorldBox = WorldBox.ExpandBy(BOX_EXPANSION_CENTIMETERS);

		const FVector2f MinScreen = ProjectPoint(WorldBox.Min);
		const FVector2f MaxScreen = ProjectPoint(WorldBox.Max);
		// WorldToScreen is monotonic per-axis (positive scale), so Min->Min and
		// Max->Max; still build the box from both to be robust to sign.
		OutGeometry.VisualBox = FBox2f(ForceInit);
		OutGeometry.VisualBox += MinScreen;
		OutGeometry.VisualBox += MaxScreen;
	}
	else
	{
		OutGeometry.VisualBox = FBox2f(ForceInit);
	}

	return true;
}
