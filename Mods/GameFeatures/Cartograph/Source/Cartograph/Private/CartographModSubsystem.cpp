#include "CartographModSubsystem.h"

#include "CartographGameInstanceModule.h"
#include "Net/CartographMapReplicator.h"

#include "GameFramework/PlayerController.h"
#include "Engine/World.h"


// The dedicated-CLIENT store binding (contract gap, see CartographMapReplicator.cpp).
// The frozen UCartographMapReplicationComponent has no public method to bind the
// client's local slim store/grid/tile-manager, so the net agent exposed it as this
// CARTOGRAPH_API free function. We forward-declare + call it from the shim.
namespace CartographNet
{
	CARTOGRAPH_API void BindClientStores(
		class UCartographMapReplicationComponent* Component,
		class FCartographBuildingStore* InStore,
		class FCartographSpatialGrid* InGrid,
		class FCartographTileManager* InTileManager);
}


ACartographModSubsystem::ACartographModSubsystem()
{
    ReplicationPolicy = ESubsystemReplicationPolicy::SpawnLocal;
    bReplicates = true;

	// Tick on the server to poll for player controllers + push per-net-tick deltas.
	// The heavy work is the replicator's; this is a light per-frame poll.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}


void ACartographModSubsystem::BeginDestroy()
{
	Super::BeginDestroy();

    CARTO_LOG("CartographModSubsystem::BeginDestroy");

    Instance = nullptr;
}


void ACartographModSubsystem::Init()
{
	Super::Init();

    CARTO_LOG("CartographModSubsystem::Init");

    Instance = this;
    CARTO_LOG_ERROR_RETURN_IF_NULL(UCartographGameInstanceModule::Instance);
    UCartographGameInstanceModule::Instance->ShouldInitialize = true;
}


void ACartographModSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UCartographGameInstanceModule* Module = UCartographGameInstanceModule::Instance;
	if (!Module)
	{
		return;
	}

	// Throttle the net poll: attaching components + diffing per-tile versions a few
	// times a second is plenty; the build hooks bump versions instantly regardless.
	NetAccumulator += DeltaSeconds;
	constexpr float NetInterval = 0.1f;  // 10 Hz
	if (NetAccumulator < NetInterval)
	{
		return;
	}
	NetAccumulator = 0.f;

	EnsureReplicationComponents();

	// Server/host: coalesce per-tile version bumps and push AoI deltas to clients.
	// No-op on a dedicated client (ServerNetTick early-outs on IsClient).
	Module->ServerNetTick();
}


void ACartographModSubsystem::EnsureReplicationComponents()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UCartographGameInstanceModule* Module = UCartographGameInstanceModule::Instance;
	if (!Module)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC)
		{
			continue;
		}

		UCartographMapReplicationComponent* ReplComp = PC->FindComponentByClass<UCartographMapReplicationComponent>();
		if (!ReplComp)
		{
			// Attach the per-PC driver. It resolves the game's ReliableMessaging
			// transport in BeginPlay (SPIKE Q1) and registers the mod-owned handlers.
			// SPIKE(Q1): the component is UCLASS(Within=PlayerController); NewObject(PC)
			// + RegisterComponent attaches it so GetTypedOuter<APlayerController>()
			// resolves the owner inside InitializeTransport. Confirm a GameFeature mod
			// may add a Within=PlayerController component on the CSS fork at this point
			// in the PC lifecycle (after BeginPlay), and that BeginPlay then fires.
			ReplComp = NewObject<UCartographMapReplicationComponent>(PC);
			ReplComp->RegisterComponent();
		}

		if (PC->HasAuthority())
		{
			// Server-role component: bind the snapshot authority so it can stream
			// shared per-tile snapshots to this client.
			ReplComp->BindServerReplicator(&Module->MapReplicator);
		}
		else
		{
			// Dedicated-client-role component: bind the local slim store/grid/tile-
			// manager so received tile blobs apply into the client's own indices.
			CartographNet::BindClientStores(ReplComp, &Module->BuildingStore, &Module->SpatialGrid, &Module->TileManager);
		}
	}
}
