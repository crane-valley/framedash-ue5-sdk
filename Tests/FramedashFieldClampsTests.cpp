// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Standalone unit tests for the pure numeric clamps in FramedashFieldClamps.h.
// No UE5 headers required -- the header is pure C++ / <cmath>. These clamps must
// mirror the ingest server limits (packages/ingest-core/src/config.ts) so an
// over-limit field never makes the consumer drop the whole flush.

#include "FramedashFieldClamps.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

using Framedash::FieldClamps::ClampFps;
using Framedash::FieldClamps::ClampMemoryBytes;
using Framedash::FieldClamps::ClampTimingMs;
using Framedash::FieldClamps::FpsFromFrameTimeMs;
using Framedash::FieldClamps::SanitizeCoord;

namespace
{
// Alias the production limits so these tests stay coupled to the header values:
// if a server cap changes, the header changes and the tests track it.
constexpr double kPositionAbsMax = Framedash::FieldClamps::PositionAbsMax;
constexpr float kTimingMsMax = Framedash::FieldClamps::TimingMsMax;
constexpr float kFpsMax = Framedash::FieldClamps::FpsMax;
constexpr int64_t kMemMax = Framedash::FieldClamps::MemoryBytesMax;
const double kQuietNaN = std::numeric_limits<double>::quiet_NaN();
const double kPosInf = std::numeric_limits<double>::infinity();
const float kQuietNaNf = std::numeric_limits<float>::quiet_NaN();
const float kPosInff = std::numeric_limits<float>::infinity();
} // namespace

// -- SanitizeCoord -----------------------------------------------------------

TEST(SanitizeCoord, NaN_ReturnsZero)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(kQuietNaN), 0.0);
}

TEST(SanitizeCoord, PositiveInfinity_ReturnsZero)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(kPosInf), 0.0);
}

TEST(SanitizeCoord, NegativeInfinity_ReturnsZero)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(-kPosInf), 0.0);
}

TEST(SanitizeCoord, ExactlyPositiveCap_PassesThrough)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(kPositionAbsMax), kPositionAbsMax);
}

TEST(SanitizeCoord, ExactlyNegativeCap_PassesThrough)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(-kPositionAbsMax), -kPositionAbsMax);
}

TEST(SanitizeCoord, AboveCap_ClampsToPositiveCap)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(kPositionAbsMax * 2.0), kPositionAbsMax);
}

TEST(SanitizeCoord, BelowCap_ClampsToNegativeCap)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(-kPositionAbsMax * 2.0), -kPositionAbsMax);
}

TEST(SanitizeCoord, NormalValue_PassesThrough)
{
	EXPECT_DOUBLE_EQ(SanitizeCoord(1234.5), 1234.5);
	EXPECT_DOUBLE_EQ(SanitizeCoord(-42.0), -42.0);
	EXPECT_DOUBLE_EQ(SanitizeCoord(0.0), 0.0);
}

// -- ClampTimingMs -----------------------------------------------------------

TEST(ClampTimingMs, NaN_ReturnsZero)
{
	EXPECT_FLOAT_EQ(ClampTimingMs(kQuietNaNf), 0.0f);
}

TEST(ClampTimingMs, Infinity_ReturnsZero)
{
	EXPECT_FLOAT_EQ(ClampTimingMs(kPosInff), 0.0f);
}

TEST(ClampTimingMs, Negative_ReturnsZero)
{
	EXPECT_FLOAT_EQ(ClampTimingMs(-1.0f), 0.0f);
}

TEST(ClampTimingMs, AboveCeiling_CapsAtMax)
{
	EXPECT_FLOAT_EQ(ClampTimingMs(kTimingMsMax + 1.0f), kTimingMsMax);
	EXPECT_FLOAT_EQ(ClampTimingMs(999999.0f), kTimingMsMax);
}

TEST(ClampTimingMs, ExactlyCeiling_PassesThrough)
{
	EXPECT_FLOAT_EQ(ClampTimingMs(kTimingMsMax), kTimingMsMax);
}

TEST(ClampTimingMs, NormalValue_PassesThrough)
{
	EXPECT_FLOAT_EQ(ClampTimingMs(16.7f), 16.7f);
	EXPECT_FLOAT_EQ(ClampTimingMs(0.0f), 0.0f);
}

// -- ClampFps ----------------------------------------------------------------

TEST(ClampFps, NaN_ReturnsZero)
{
	EXPECT_FLOAT_EQ(ClampFps(kQuietNaNf), 0.0f);
}

TEST(ClampFps, Infinity_ReturnsZero)
{
	EXPECT_FLOAT_EQ(ClampFps(kPosInff), 0.0f);
}

TEST(ClampFps, Negative_ReturnsZero)
{
	EXPECT_FLOAT_EQ(ClampFps(-30.0f), 0.0f);
}

TEST(ClampFps, AboveCeiling_CapsAtMax)
{
	EXPECT_FLOAT_EQ(ClampFps(kFpsMax + 500.0f), kFpsMax);
}

TEST(ClampFps, ExactlyCeiling_PassesThrough)
{
	EXPECT_FLOAT_EQ(ClampFps(kFpsMax), kFpsMax);
}

TEST(ClampFps, NormalValue_PassesThrough)
{
	EXPECT_FLOAT_EQ(ClampFps(60.0f), 60.0f);
	EXPECT_FLOAT_EQ(ClampFps(0.0f), 0.0f);
}

// -- FpsFromFrameTimeMs ------------------------------------------------------

TEST(FpsFromFrameTimeMs, ZeroFrameTime_ReturnsZero)
{
	EXPECT_FLOAT_EQ(FpsFromFrameTimeMs(0.0f), 0.0f);
}

TEST(FpsFromFrameTimeMs, NegativeFrameTime_ReturnsZero)
{
	EXPECT_FLOAT_EQ(FpsFromFrameTimeMs(-1.0f), 0.0f);
}

TEST(FpsFromFrameTimeMs, NaNFrameTime_ReturnsZero)
{
	EXPECT_FLOAT_EQ(FpsFromFrameTimeMs(kQuietNaNf), 0.0f);
}

TEST(FpsFromFrameTimeMs, SubMillisecondFrame_CapsAt1000)
{
	// 0.5ms frame -> 2000 fps uncapped -> capped to 1000.
	EXPECT_FLOAT_EQ(FpsFromFrameTimeMs(0.5f), kFpsMax);
}

TEST(FpsFromFrameTimeMs, NormalFrame_ComputesFps)
{
	// 16.666... ms -> ~60 fps.
	EXPECT_NEAR(FpsFromFrameTimeMs(1000.0f / 60.0f), 60.0f, 1e-3f);
	// 10ms -> 100 fps.
	EXPECT_FLOAT_EQ(FpsFromFrameTimeMs(10.0f), 100.0f);
}

// -- ClampMemoryBytes --------------------------------------------------------

TEST(ClampMemoryBytes, Negative_ReturnsZero)
{
	EXPECT_EQ(ClampMemoryBytes(-1), 0);
	EXPECT_EQ(ClampMemoryBytes(std::numeric_limits<int64_t>::min()), 0);
}

TEST(ClampMemoryBytes, Zero_PassesThrough)
{
	EXPECT_EQ(ClampMemoryBytes(0), 0);
}

TEST(ClampMemoryBytes, Positive_PassesThrough)
{
	EXPECT_EQ(ClampMemoryBytes(1024), 1024);
	// A reading just under the cap is unchanged.
	EXPECT_EQ(ClampMemoryBytes(kMemMax - 1), kMemMax - 1);
}

TEST(ClampMemoryBytes, AboveCap_CapsAt64GiB)
{
	EXPECT_EQ(ClampMemoryBytes(kMemMax), kMemMax);
	EXPECT_EQ(ClampMemoryBytes(kMemMax + 1), kMemMax);
	EXPECT_EQ(ClampMemoryBytes(std::numeric_limits<int64_t>::max()), kMemMax);
}
