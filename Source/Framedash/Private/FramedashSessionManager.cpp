// Copyright Crane Valley. All Rights Reserved.

#include "FramedashSessionManager.h"
#include "Framedash.h"
#include "FramedashUuid.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Timespan.h"

namespace
{

// Ticks (100ns) between FDateTime epoch (0001-01-01T00:00:00Z) and Unix
// epoch (1970-01-01T00:00:00Z). Matches FDateTime::ToUnixTimestamp.
constexpr int64 UnixEpochInDateTimeTicks = 621355968000000000;

Framedash::FXoshiro256pp& GetSessionIdRng()
{
	thread_local Framedash::FXoshiro256pp Rng = []()
	{
		Framedash::FXoshiro256pp R;
		const FGuid G1 = FGuid::NewGuid();
		const FGuid G2 = FGuid::NewGuid();
		R.Seed(
			(static_cast<uint64>(G1.A) << 32) | static_cast<uint64>(G1.B),
			(static_cast<uint64>(G1.C) << 32) | static_cast<uint64>(G1.D),
			(static_cast<uint64>(G2.A) << 32) | static_cast<uint64>(G2.B),
			(static_cast<uint64>(G2.C) << 32) | static_cast<uint64>(G2.D));
		return R;
	}();
	return Rng;
}

FString NewSessionIdV7()
{
	const uint64 UnixTsMs = static_cast<uint64>(
		(FDateTime::UtcNow().GetTicks() - UnixEpochInDateTimeTicks) / ETimespan::TicksPerMillisecond);

	// Capture Next() into named locals: C++17 leaves function-argument
	// evaluation order unspecified, and using the same generator twice
	// inline would let a compiler swap rand_a / rand_b sources between
	// builds. The UUID stays valid either way, but pinning the order
	// keeps captured logs reproducible against a fixed seed.
	Framedash::FXoshiro256pp& Rng = GetSessionIdRng();
	const uint64 R1 = Rng.Next();
	const uint64 R2 = Rng.Next();
	const Framedash::FUuidFields Fields = Framedash::PackUuidV7(UnixTsMs, R1, R2);

	return FGuid(Fields.A, Fields.B, Fields.C, Fields.D)
		.ToString(EGuidFormats::DigitsWithHyphensLower);
}

} // namespace

FFramedashSessionManager::FFramedashSessionManager(const FString& InPlayerId)
	: PlayerId(InPlayerId.TrimStartAndEnd())
{
	SessionId = NewSessionIdV7();
	UE_LOG(LogFramedash, Log, TEXT("Session: %s, Player: %s"), *SessionId, PlayerId.IsEmpty() ? TEXT("(anonymous)") : TEXT("(set)"));
}

void FFramedashSessionManager::SetPlayerId(const FString& NewPlayerId)
{
	check(IsInGameThread());
	PlayerId = NewPlayerId.TrimStartAndEnd();
}
