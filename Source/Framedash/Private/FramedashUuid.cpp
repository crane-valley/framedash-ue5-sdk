// Copyright Crane Valley. All Rights Reserved.

#include "FramedashUuid.h"

namespace Framedash
{

namespace
{

constexpr uint64_t Rotl(uint64_t X, int K)
{
	return (X << K) | (X >> (64 - K));
}

} // namespace

void FXoshiro256pp::Seed(uint64_t S0, uint64_t S1, uint64_t S2, uint64_t S3)
{
	S[0] = S0;
	S[1] = S1;
	S[2] = S2;
	S[3] = S3;
}

uint64_t FXoshiro256pp::Next()
{
	const uint64_t Result = Rotl(S[0] + S[3], 23) + S[0];
	const uint64_t T = S[1] << 17;

	S[2] ^= S[0];
	S[3] ^= S[1];
	S[1] ^= S[2];
	S[0] ^= S[3];

	S[2] ^= T;
	S[3] = Rotl(S[3], 45);

	return Result;
}

FUuidFields PackUuidV7(uint64_t UnixTsMs, uint64_t R1, uint64_t R2)
{
	UnixTsMs &= 0x0000FFFFFFFFFFFFULL;

	const uint32_t RandA   = static_cast<uint32_t>(R1 & 0x0FFFu);
	const uint32_t RandBHi = static_cast<uint32_t>((R1 >> 12) & 0x3FFFu);
	const uint32_t RandBMd = static_cast<uint32_t>((R2 >> 32) & 0xFFFFu);
	const uint32_t RandBLo = static_cast<uint32_t>(R2 & 0xFFFFFFFFu);

	FUuidFields U{};
	U.A = static_cast<uint32_t>(UnixTsMs >> 16);
	U.B = (static_cast<uint32_t>(UnixTsMs & 0xFFFFu) << 16)
		| (0x7000u | RandA);
	U.C = ((0x8000u | RandBHi) << 16) | RandBMd;
	U.D = RandBLo;
	return U;
}

} // namespace Framedash
