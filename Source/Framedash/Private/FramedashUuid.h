// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

// Engine-independent: this header MUST NOT include UE5 headers
// (CoreMinimal.h, Misc/Guid.h, etc.). Pure C++ so it can be compiled
// into the standalone GoogleTest harness under sdks/ue5/Tests without
// an UnrealEditor build.

#include <cstdint>

namespace Framedash
{

// FGuid-compatible 4x32 layout. Constructing FGuid(A, B, C, D) and
// calling ToString(EGuidFormats::DigitsWithHyphensLower) on the result
// produces the canonical 8-4-4-4-12 hex string defined by RFC 9562.
struct FUuidFields
{
	uint32_t A;
	uint32_t B;
	uint32_t C;
	uint32_t D;
};

// Xoshiro256++ PRNG (Blackman/Vigna). Seeded externally; thread safety
// is the caller's responsibility (SDK uses thread_local instances).
// An all-zero seed produces an all-zero stream, so callers must seed
// with non-trivial entropy before calling Next().
class FXoshiro256pp
{
public:
	FXoshiro256pp() = default;

	void Seed(uint64_t S0, uint64_t S1, uint64_t S2, uint64_t S3);

	uint64_t Next();

private:
	uint64_t S[4]{};
};

FUuidFields PackUuidV7(uint64_t UnixTsMs, uint64_t R1, uint64_t R2);

}
