#pragma once

#include "Subsystem/ModSubsystem.h"

#include "CoreMinimal.h"

#include "CartographModSubsystem.generated.h"


class UCartographGameInstanceModule;


/**
 * Cartograph server/host mod subsystem.
 *
 * POST-rearchitecture this is a THIN SHIM (SPEC 4.4 / 4.5):
 *  - It still flips UCartographGameInstanceModule::ShouldInitialize so the gather
 *    runs once the world + subsystem exist.
 *  - The legacy per-event reliable NetMulticast (ClientUpdateBuildingData with a
 *    full TArray<FBuildingData>) is DELETED. All transfer now goes through
 *    FCartographMapReplicator + UCartographMapReplicationComponent over the in-repo
 *    ReliableMessaging transport (versioned per-tile AoI deltas), so there is no
 *    multicast-reliable-flood and no second full dataset on the wire.
 *  - On the server it Tick-drives the per-net-tick delta push
 *    (UCartographGameInstanceModule::ServerNetTick) and attaches a
 *    UCartographMapReplicationComponent to each player controller.
 */
UCLASS(Transient)
class CARTOGRAPH_API ACartographModSubsystem : public AModSubsystem
{
	GENERATED_BODY()

    friend class UCartographGameInstanceModule;

public:
	ACartographModSubsystem();

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginDestroy() override;

protected:
	virtual void Init() override;

private:
	/** Ensure each player controller has a UCartographMapReplicationComponent and
	 *  bind the server-side replicator to the server-role ones. Idempotent; polled
	 *  from Tick so late-joining PCs are picked up. */
	void EnsureReplicationComponents();

	/** Phase-1 transport probe (r.Cartograph.Net.ProbeAoI). On a client, re-emits a
	 *  whole-world RequestAoI ~1/s for a bounded window so a pre-Connected (silently
	 *  buffered) request cannot false-negative. Diagnostic; default-off no-op. */
	void MaybeDriveAoIProbe();

protected:
	inline static ACartographModSubsystem* Instance = nullptr;

	/** Throttle for EnsureReplicationComponents / version-push cadence. */
	float NetAccumulator = 0.f;

	/** Bounded 10 Hz tick counter for the ProbeAoI retry window (reset when the CVar
	 *  is off, so toggling it 0->1 re-arms the window). */
	int32 ProbeTickCounter = 0;
};
