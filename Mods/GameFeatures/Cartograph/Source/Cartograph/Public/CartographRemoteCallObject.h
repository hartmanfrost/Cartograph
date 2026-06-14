#pragma once

#include "FGRemoteCallObject.h"

#include "CoreMinimal.h"

#include "CartographRemoteCallObject.generated.h"


// =============================================================================
// UCartographRemoteCallObject - DEPRECATED (SPEC 4.4).
// -----------------------------------------------------------------------------
// The hand-rolled slice-RCO transport is DELETED. It was where crash #9 lived:
//   - three unchecked TMap::Find() null-derefs racing the 10 s per-slice timeout
//     (rco-null-deref-timeout-race),
//   - an int16 slice-count overflow at ~63 MB (int16-sliceindex-overflow),
//   - a fixed-2047-byte stack-tail leak (netserialize-fixed-2047-waste),
//   - a per-join synchronous `Archive << CurrentBuildingData` on the game thread
//     (sync-full-serialize-per-join-hitch).
//
// ALL transfer now routes through FCartographMapReplicator +
// UCartographMapReplicationComponent over the in-repo ReliableMessaging plugin
// (int32 sizes / uint64 ids -> the #9 bug class is gone by construction).
//
// This class is reduced to a NO-OP stub. It is kept (rather than removed) so any
// dangling reference / save-embedded RCO class path still resolves and the UCLASS
// still compiles. It carries no slice protocol, no buffers, no timers, no full
// dataset serialize. It can be deleted outright once it is confirmed nothing in
// content / SML references the class.
// =============================================================================
UCLASS()
class CARTOGRAPH_API UCartographRemoteCallObject : public UFGRemoteCallObject
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	// FGRemoteCallObject requires at least one replicated property to register the
	// object on the connection; keep a single dummy so the UCLASS stays valid.
	UPROPERTY(Replicated)
	bool bDummy = true;
};
