// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashSessionManager.h"
#include "Framedash.h"
#include "FramedashTypes.h"
#include "FramedashStringUtil.h"
#include "FramedashUuid.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Timespan.h"

namespace
{

// Ticks (100ns) between FDateTime epoch (0001-01-01T00:00:00Z) and Unix
// epoch (1970-01-01T00:00:00Z). Matches FDateTime::ToUnixTimestamp.
constexpr int64 UnixEpochInDateTimeTicks = 621355968000000000;

// Trim whitespace and truncate to the ingest player_id cap. An over-limit
// player_id is rejected by ingest validation, which drops the whole batch, so
// the SDK normalizes it before storing. Truncation goes through
// TruncateToUtf16Units so it counts UTF-16 code units (the server's JS
// string-length unit) correctly on both 2- and 4-byte TCHAR builds, mirroring
// the Godot/Unity C# SDKs.
FString NormalizePlayerId(const FString& PlayerId)
{
	// Truncate by UTF-16 code units (ingest's unit), surrogate-safe and
	// platform-correct. See FramedashStringUtil.h.
	return Framedash::TruncateToUtf16Units(
		PlayerId.TrimStartAndEnd(), FramedashConstants::MaxPlayerIdLength);
}

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
	: PlayerId(NormalizePlayerId(InPlayerId))
{
	SessionId = NewSessionIdV7();
	UE_LOG(LogFramedash, Log, TEXT("Session: %s, Player: %s"), *SessionId, PlayerId.IsEmpty() ? TEXT("(anonymous)") : TEXT("(set)"));
}

void FFramedashSessionManager::SetPlayerId(const FString& NewPlayerId)
{
	// Graceful guard, NOT a fatal check(): the SDK's hard rule is to never crash
	// the game. SetPlayerId mutates PlayerId without synchronization and must run
	// on the game thread. The early-return is gated on an explicit IsInGameThread()
	// so control flow never depends on whether ensure is compiled in; ensureMsgf is
	// used only to surface the misuse with a callstack in development. Both are
	// non-fatal, so a mis-threaded call is ignored rather than crashing.
	if (!IsInGameThread())
	{
		ensureMsgf(false, TEXT("SetPlayerId must be called from the game thread; off-thread call ignored."));
		return;
	}
	PlayerId = NormalizePlayerId(NewPlayerId);
}
