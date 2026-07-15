// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Standalone unit tests for Framedash::PackUuidV7 and FXoshiro256pp.
// Verifies RFC 9562 bit layout and PRNG determinism without requiring
// an UnrealEditor build.

#include "FramedashUuid.h"

#include <cstdint>
#include <gtest/gtest.h>

using Framedash::FUuidFields;
using Framedash::FXoshiro256pp;
using Framedash::PackUuidV7;

namespace
{

constexpr uint64_t kVersionMaskInB = 0xF000u;
constexpr uint64_t kVersion7InB    = 0x7000u;
constexpr uint64_t kVariantMaskInC = 0xC0000000u;
constexpr uint64_t kVariant10InC   = 0x80000000u;

uint64_t Reconstruct48BitTimestamp(const FUuidFields& U)
{
	return (static_cast<uint64_t>(U.A) << 16) | static_cast<uint64_t>(U.B >> 16);
}

} // namespace

// -- Bit layout --------------------------------------------------------

TEST(UuidV7, VersionNibbleIsSeven)
{
	const FUuidFields U = PackUuidV7(0, 0, 0);
	EXPECT_EQ(U.B & kVersionMaskInB, kVersion7InB);
}

TEST(UuidV7, VariantBitsAreOneZero)
{
	const FUuidFields U = PackUuidV7(0, 0, 0);
	EXPECT_EQ(U.C & kVariantMaskInC, kVariant10InC);
}

TEST(UuidV7, VersionAndVariantHoldForArbitraryRandomInputs)
{
	const FUuidFields U = PackUuidV7(0x1234567890ABULL,
		0xFFFFFFFFFFFFFFFFULL,
		0xFFFFFFFFFFFFFFFFULL);
	EXPECT_EQ(U.B & kVersionMaskInB, kVersion7InB);
	EXPECT_EQ(U.C & kVariantMaskInC, kVariant10InC);
}

// -- Timestamp ---------------------------------------------------------

TEST(UuidV7, TimestampZeroProducesAllZeroPrefix)
{
	const FUuidFields U = PackUuidV7(0, 0, 0);
	EXPECT_EQ(U.A, 0u);
	EXPECT_EQ(U.B >> 16, 0u);
}

TEST(UuidV7, TimestampRoundTrips48Bits)
{
	const uint64_t Ts = 0x0123456789ABULL;
	const FUuidFields U = PackUuidV7(Ts, 0, 0);
	EXPECT_EQ(Reconstruct48BitTimestamp(U), Ts);
}

TEST(UuidV7, TimestampMaxValueRoundTrips)
{
	const uint64_t Ts = 0x0000FFFFFFFFFFFFULL;
	const FUuidFields U = PackUuidV7(Ts, 0, 0);
	EXPECT_EQ(Reconstruct48BitTimestamp(U), Ts);
}

TEST(UuidV7, TimestampAbove48BitsIsTruncated)
{
	const uint64_t TsRaw = 0xABCD0123456789ABULL;
	const uint64_t TsTruncated = TsRaw & 0x0000FFFFFFFFFFFFULL;
	const FUuidFields U = PackUuidV7(TsRaw, 0, 0);
	EXPECT_EQ(Reconstruct48BitTimestamp(U), TsTruncated);
}

TEST(UuidV7, EarlierTimestampSortsBeforeLater)
{
	const FUuidFields Earlier = PackUuidV7(1000, 0, 0);
	const FUuidFields Later   = PackUuidV7(2000, 0, 0);
	EXPECT_LT(Reconstruct48BitTimestamp(Earlier), Reconstruct48BitTimestamp(Later));
}

// -- Random material placement -----------------------------------------

TEST(UuidV7, RandAComesFromR1Low12Bits)
{
	// R1 = 0xABC -> rand_a = 0xABC. R2 = 0 keeps rand_b deterministic.
	const FUuidFields U = PackUuidV7(0, 0xABCULL, 0);
	EXPECT_EQ(U.B & 0x0FFFu, 0xABCu);
}

TEST(UuidV7, RandBHiComesFromR1Bits12Through25)
{
	// 14 bits at offset 12: 0x3FFF << 12 = 0x3FFF000.
	const FUuidFields U = PackUuidV7(0, 0x3FFF000ULL, 0);
	EXPECT_EQ((U.C >> 16) & 0x3FFFu, 0x3FFFu);
	EXPECT_EQ(U.B & 0x0FFFu, 0u);
}

TEST(UuidV7, RandBMdComesFromR2Bits32Through47)
{
	// R2 = 0xCAFE_0000_0000_0000 places 0xCAFE at bits 32..47.
	const FUuidFields U = PackUuidV7(0, 0, 0xCAFE00000000ULL);
	EXPECT_EQ(U.C & 0xFFFFu, 0xCAFEu);
}

TEST(UuidV7, RandBLoComesFromR2Low32Bits)
{
	const FUuidFields U = PackUuidV7(0, 0, 0xDEADBEEFULL);
	EXPECT_EQ(U.D, 0xDEADBEEFu);
}

TEST(UuidV7, RandomFieldsIndependentOfTimestamp)
{
	const FUuidFields A = PackUuidV7(1000, 0xABCULL, 0xDEADBEEFULL);
	const FUuidFields B = PackUuidV7(9999, 0xABCULL, 0xDEADBEEFULL);
	EXPECT_EQ(A.B & 0x0FFFu, B.B & 0x0FFFu);
	EXPECT_EQ(A.C, B.C);
	EXPECT_EQ(A.D, B.D);
}

// -- Xoshiro256pp ------------------------------------------------------

TEST(Xoshiro256pp, DefaultConstructedZeroSeedYieldsZeroStream)
{
	FXoshiro256pp Rng;
	EXPECT_EQ(Rng.Next(), 0u);
	EXPECT_EQ(Rng.Next(), 0u);
}

TEST(Xoshiro256pp, NonZeroSeedYieldsNonZeroOutput)
{
	FXoshiro256pp Rng;
	Rng.Seed(1, 2, 3, 4);
	const uint64_t First = Rng.Next();
	const uint64_t Second = Rng.Next();
	EXPECT_NE(First, 0u);
	EXPECT_NE(Second, 0u);
	EXPECT_NE(First, Second);
}

TEST(Xoshiro256pp, SameSeedProducesSameSequence)
{
	FXoshiro256pp A;
	A.Seed(0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL,
		0x94D049BB133111EBULL, 0x6A09E667F3BCC908ULL);

	FXoshiro256pp B;
	B.Seed(0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL,
		0x94D049BB133111EBULL, 0x6A09E667F3BCC908ULL);

	for (int I = 0; I < 32; ++I)
	{
		EXPECT_EQ(A.Next(), B.Next()) << "diverged at iteration " << I;
	}
}

TEST(Xoshiro256pp, DifferentSeedsProduceDifferentSequences)
{
	FXoshiro256pp A;
	A.Seed(1, 2, 3, 4);

	FXoshiro256pp B;
	B.Seed(5, 6, 7, 8);

	bool bAnyDifferent = false;
	for (int I = 0; I < 16; ++I)
	{
		if (A.Next() != B.Next())
		{
			bAnyDifferent = true;
			break;
		}
	}
	EXPECT_TRUE(bAnyDifferent);
}

// Known-answer vector: lock the algorithm against accidental rotation /
// XOR-cascade reorderings. The expected first value is hand-derived from
// the canonical Blackman/Vigna reference for seed (1, 2, 3, 4):
//   result = rotl(s0 + s3, 23) + s0 = rotl(5, 23) + 1 = 0x2800000 + 1.
TEST(Xoshiro256pp, KnownAnswerVectorForSeed_1_2_3_4)
{
	FXoshiro256pp Rng;
	Rng.Seed(1, 2, 3, 4);
	EXPECT_EQ(Rng.Next(), 0x0000000002800001ULL);
}
