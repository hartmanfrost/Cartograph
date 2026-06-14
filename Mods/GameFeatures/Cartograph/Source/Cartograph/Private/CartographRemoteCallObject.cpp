#include "CartographRemoteCallObject.h"

#include "Net/UnrealNetwork.h"

// =============================================================================
// CartographRemoteCallObject.cpp - DEPRECATED no-op stub (SPEC 4.4).
// The slice protocol (ServerRequestInitialBuildingData / ClientReceiveInitialBuildingData
// / InitialBuildableDeserialize / FBuildingDataBuffer NetSerialize) is DELETED.
// All transfer routes through FCartographMapReplicator now. Only the minimal UCLASS
// boilerplate remains so the class still resolves and compiles.
// =============================================================================

void UCartographRemoteCallObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME(UCartographRemoteCallObject, bDummy);
}
