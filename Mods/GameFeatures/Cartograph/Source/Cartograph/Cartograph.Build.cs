using UnrealBuildTool;
using System.IO;
using System;

public class Cartograph : ModuleRules
{
	public Cartograph(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// ---------------------------------------------------------------------
		// Cartograph re-architecture (SPEC STAGE2) build feature flag.
		// CARTOGRAPH_WITH_ABSTRACTINSTANCE gates the Phase-4 (InstancedCapture /
		// "C") render backend code (CartographInstanceRenderer.{h,cpp}) so a build
		// WITHOUT the AbstractInstance plugin still compiles. The TiledCanvas
		// (Phase 2 / A) and SlateInstanced (Phase 3 / B) backends never depend on
		// AbstractInstance and remain the always-available default + fallback.
		//
		// Q6 caveat (SPEC 7 / 10): AbstractInstance's own AbstractInstance.Build.cs
		// lists AkAudio / Wwise as a dependency, so adding "AbstractInstance" to the
		// plugin deps below pulls a heavyweight audio plugin transitively into this
		// module's link. Enabling Phase 4 is therefore NOT a one-line uncomment - it
		// is a deliberate prerequisite. Until that link + the no-op no-Ak manager
		// (Q6) + capture isolation (Q5) + high-churn throughput (Q7) are validated,
		// keep the plugin dep commented AND the define 0 so the gated code is not
		// compiled. To enable Phase 4: uncomment "AbstractInstance" in the plugin
		// list, set this define to 1, and accept the transitive Wwise link.
		bool bCartographWithAbstractInstance = false;
		PublicDefinitions.Add("CARTOGRAPH_WITH_ABSTRACTINSTANCE=" + (bCartographWithAbstractInstance ? "1" : "0"));

		// FactoryGame transitive dependencies
		// Not all of these are required, but including the extra ones saves you from having to add them later.
		// Some entries are commented out to avoid compile-time warnings about depending on a module that you don't explicitly depend on.
		// You can uncomment these as necessary when your code actually needs to use them.
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			"InputCore",
			//"OnlineSubsystem", "OnlineSubsystemUtils", "OnlineSubsystemNull",
			//"SignificanceManager",
			"GeometryCollectionEngine",
			//"ChaosVehiclesCore", "ChaosVehicles", "ChaosSolverEngine",
			"AnimGraphRuntime",
			//"AkAudio",
			"AssetRegistry",
			"NavigationSystem",
			//"ReplicationGraph",
			"AIModule",
			"GameplayTasks",
			// Slate stack: required by the Phase-3 SlateInstanced backend
			// (SCartographMapView: SLeafWidget + FSlateDrawElement::MakeCustomVerts)
			// and the existing UMG menu widgets.
			"SlateCore", "Slate", "UMG",
			//"InstancedSplines",
			// RenderCore + RHI (RHI in PrivateDependencyModuleNames below): the
			// Phase-2 TiledCanvas compositor's scissored hardware clear + the
			// ELoad-via-RHI Phase-A fallback (SPEC Q3) need these.
			"RenderCore",
			"CinematicCamera",
			"Foliage",
			//"Niagara",
			//"EnhancedInput",
			//"GameplayCameras",
			//"TemplateSequence",
			"NetCore",
			"GameplayTags",
			"Json", "JsonUtilities"
		});

		// FactoryGame plugins
		PublicDependencyModuleNames.AddRange(new string[] {
			//"AbstractInstance",        // Q6: pulls AkAudio/Wwise transitively. Uncomment
			                             //     ONLY together with bCartographWithAbstractInstance
			                             //     = true above to enable the Phase-4 backend.
			//"InstancedSplinesComponent",
			//"SignificanceISPC"
		});

		// In-repo ReliableMessaging plugin: the entire network transport for the
		// re-architected join + per-tile delta path (SPEC 4.4). Replaces the deleted
		// hand-rolled slice RCO. UReliableMessagingPlayerComponent::GetFromPlayer +
		// SendTaggedMessage + RegisterTaggedMessageHandler are used by
		// CartographMapReplicator.cpp. SPIKE(Q1): the mod-wiring path (auto-attached
		// component? Connected without mod handshake? mod-owned tag?) is unverified.
		PublicDependencyModuleNames.AddRange(new string[] {
			"ReliableMessaging",
		});

		// Conditionally add the AbstractInstance plugin dependency for Phase 4.
		// Mirrors bCartographWithAbstractInstance so flipping the flag wires the link
		// without editing two places.
		if (bCartographWithAbstractInstance)
		{
			PublicDependencyModuleNames.Add("AbstractInstance");
		}

		// Header stubs
		PublicDependencyModuleNames.AddRange(new string[] {
			"DummyHeaders",
		});

		if (Target.Type == TargetRules.TargetType.Editor) {
			PublicDependencyModuleNames.AddRange(new string[] {/*"OnlineBlueprintSupport",*/ "AnimGraph"});
		}
		PublicDependencyModuleNames.AddRange(new string[] {"FactoryGame", "SML"});
		
		PublicIncludePaths.AddRange(new string[] {
			// ... add public include paths required here ...
		});
		
		PrivateIncludePaths.AddRange(new string[] {
			// ... add private include paths required here ...
		});
		
		PublicDependencyModuleNames.AddRange(new string[] {
			// ... add public dependencies that you statically link with here ...
		});
		
		PrivateDependencyModuleNames.AddRange(new string[] {
			"RHI"
		});
		
		DynamicallyLoadedModuleNames.AddRange(new string[] {
			// ... add any modules that your module loads dynamically here ...
		});
	}
}
