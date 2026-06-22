// Copyright Crane Valley. All Rights Reserved.
//
// Standalone unit tests for Framedash::NormalizeYawDegrees and
// Framedash::NormalizePitchDegrees in FramedashCameraMath.h.
// No UE5 headers required -- the math header is pure C++ / <cmath>.

#include "FramedashCameraMath.h"

#include <gtest/gtest.h>

using Framedash::NormalizeYawDegrees;
using Framedash::NormalizePitchDegrees;

// -- NormalizeYawDegrees -----------------------------------------------------

TEST(NormalizeYawDegrees, Zero_ReturnsZero)
{
	EXPECT_FLOAT_EQ(NormalizeYawDegrees(0.0f), 0.0f);
}

TEST(NormalizeYawDegrees, JustUnder360_Unchanged)
{
	EXPECT_NEAR(NormalizeYawDegrees(359.9f), 359.9f, 1e-4f);
}

TEST(NormalizeYawDegrees, Exactly360_FoldsToZero)
{
	EXPECT_FLOAT_EQ(NormalizeYawDegrees(360.0f), 0.0f);
}

TEST(NormalizeYawDegrees, Over360_WrapsCorrectly)
{
	EXPECT_NEAR(NormalizeYawDegrees(370.0f), 10.0f, 1e-4f);
}

TEST(NormalizeYawDegrees, Negative10_Maps350)
{
	EXPECT_NEAR(NormalizeYawDegrees(-10.0f), 350.0f, 1e-4f);
}

TEST(NormalizeYawDegrees, ExactlyTwoRevolutions_ReturnsZero)
{
	EXPECT_FLOAT_EQ(NormalizeYawDegrees(720.0f), 0.0f);
}

TEST(NormalizeYawDegrees, Negative370_Maps350)
{
	EXPECT_NEAR(NormalizeYawDegrees(-370.0f), 350.0f, 1e-4f);
}

TEST(NormalizeYawDegrees, TinyNegative_StaysBelow360)
{
	// A tiny negative input whose +360 rounds to exactly 360.0f on the float cast
	// must fold back into the half-open [0, 360) wire range (not return 360.0f).
	const float Y = NormalizeYawDegrees(-1e-7);
	EXPECT_GE(Y, 0.0f);
	EXPECT_LT(Y, 360.0f);
}

// -- NormalizePitchDegrees ---------------------------------------------------

TEST(NormalizePitchDegrees, Zero_ReturnsZero)
{
	EXPECT_FLOAT_EQ(NormalizePitchDegrees(0.0f), 0.0f);
}

TEST(NormalizePitchDegrees, Positive45_Unchanged)
{
	EXPECT_FLOAT_EQ(NormalizePitchDegrees(45.0f), 45.0f);
}

TEST(NormalizePitchDegrees, Positive90_AtCeiling)
{
	EXPECT_FLOAT_EQ(NormalizePitchDegrees(90.0f), 90.0f);
}

TEST(NormalizePitchDegrees, SlightlyOver90_ClampsTo90)
{
	EXPECT_FLOAT_EQ(NormalizePitchDegrees(95.0f), 90.0f);
}

TEST(NormalizePitchDegrees, Negative95_ClampsToNeg90)
{
	EXPECT_FLOAT_EQ(NormalizePitchDegrees(-95.0f), -90.0f);
}

TEST(NormalizePitchDegrees, UE5Encoded350_FoldsToNeg10)
{
	// UE FRotator stores looking-down pitch as e.g. 350 degrees (positive-up convention).
	// 350 - 360 = -10, which is within [-90, 90].
	EXPECT_NEAR(NormalizePitchDegrees(350.0f), -10.0f, 1e-4f);
}

TEST(NormalizePitchDegrees, Positive30_Unchanged)
{
	EXPECT_FLOAT_EQ(NormalizePitchDegrees(30.0f), 30.0f);
}
