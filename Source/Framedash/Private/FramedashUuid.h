// Copyright Crane Valley. All Rights Reserved.

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

// Pack a UUIDv7 (RFC 9562) from a millisecond timestamp and 128 bits
// of random material. Bit layout (most significant bit first):
//   bits   0..47  unix_ts_ms (input is truncated to 48 bits)
//   bits  48..51  ver = 0x7
//   bits  52..63  rand_a (12 bits, taken from R1[0..11])
//   bits  64..65  var = 0b10
//   bits  66..127 rand_b (62 bits)
// Of R1 the low 26 bits are consumed (12 for rand_a, 14 for rand_b
// high). Of R2 the low 32 bits and bits 32..47 are consumed (32+16
// for rand_b mid/low). Remaining input bits are ignored.
FUuidFields PackUuidV7(uint64_t UnixTsMs, uint64_t R1, uint64_t R2);

} // namespace Framedash
