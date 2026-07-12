// Copyright Crane Valley. All Rights Reserved.
//
// Standalone unit tests for the pure C++ map/level load-time timer in
// FramedashMapLoadTimer.h. No UE5 headers required -- the header is pure C++ /
// <cmath>. The timer holds the pending measurement's monotonic start timestamp
// and computes the elapsed load time (ms) attached to the map_load event's
// metrics map; these tests lock in the Begin/End timing, replace-on-double-Begin,
// End-without-Begin no-op, backwards-clock floor, and the drop-not-clamp
// validation used by the direct ReportMapLoad feed. The subsystem wiring (map_id
// = map name, metrics["load_time_ms"] via TrackWithData) is UE-coupled and
// covered separately (engine automation), mirroring the FramedashIoStats split.

#include "FramedashMapLoadTimer.h"

#include <limits>

#include <gtest/gtest.h>

using Framedash::FMapLoadTimer;

namespace
{
const double kQuietNaN = std::numeric_limits<double>::quiet_NaN();
const double kPosInf = std::numeric_limits<double>::infinity();
const double kNegInf = -std::numeric_limits<double>::infinity();
} // namespace

// -- Begin / End timing ------------------------------------------------------

TEST(MapLoadTimer, EndAfterBeginComputesElapsedMs)
{
	FMapLoadTimer Timer;
	Timer.Begin(10.0);

	double ElapsedMs = -1.0;
	const bool bCompleted = Timer.End(12.5, ElapsedMs);

	EXPECT_TRUE(bCompleted);
	EXPECT_DOUBLE_EQ(ElapsedMs, 2500.0);
}

TEST(MapLoadTimer, SubSecondLoadComputesFractionalMs)
{
	FMapLoadTimer Timer;
	Timer.Begin(100.0);

	double ElapsedMs = 0.0;
	Timer.End(100.25, ElapsedMs);

	EXPECT_DOUBLE_EQ(ElapsedMs, 250.0);
}

// -- End without Begin is a no-op --------------------------------------------

TEST(MapLoadTimer, EndWithoutBeginReturnsFalseNoOp)
{
	FMapLoadTimer Timer;

	double ElapsedMs = -1.0;
	const bool bCompleted = Timer.End(5.0, ElapsedMs);

	EXPECT_FALSE(bCompleted);
	EXPECT_DOUBLE_EQ(ElapsedMs, 0.0);
}

TEST(MapLoadTimer, EndTwiceSecondReturnsFalse)
{
	FMapLoadTimer Timer;
	Timer.Begin(1.0);

	double ElapsedMs = 0.0;
	EXPECT_TRUE(Timer.End(2.0, ElapsedMs));
	// Pending state cleared after the first End.
	EXPECT_FALSE(Timer.End(3.0, ElapsedMs));
	EXPECT_DOUBLE_EQ(ElapsedMs, 0.0);
}

// -- Begin again before End replaces the pending measurement -----------------

TEST(MapLoadTimer, BeginTwiceReplacesPendingMeasurement)
{
	FMapLoadTimer Timer;
	Timer.Begin(10.0);
	Timer.Begin(20.0);

	double ElapsedMs = 0.0;
	const bool bCompleted = Timer.End(21.0, ElapsedMs);

	EXPECT_TRUE(bCompleted);
	// The second Begin wins: elapsed is measured from the later start.
	EXPECT_DOUBLE_EQ(ElapsedMs, 1000.0);
}

// -- Backwards clock is floored at 0, never negative -------------------------

TEST(MapLoadTimer, BackwardsClockFloorsElapsedAtZero)
{
	FMapLoadTimer Timer;
	Timer.Begin(50.0);

	double ElapsedMs = -1.0;
	Timer.End(49.0, ElapsedMs);

	EXPECT_DOUBLE_EQ(ElapsedMs, 0.0);
}

// -- HasPending / Reset lifecycle --------------------------------------------

TEST(MapLoadTimer, HasPendingTracksLifecycle)
{
	FMapLoadTimer Timer;
	EXPECT_FALSE(Timer.HasPending());

	Timer.Begin(0.0);
	EXPECT_TRUE(Timer.HasPending());

	double ElapsedMs = 0.0;
	Timer.End(1.0, ElapsedMs);
	EXPECT_FALSE(Timer.HasPending());
}

TEST(MapLoadTimer, ResetClearsPending)
{
	FMapLoadTimer Timer;
	Timer.Begin(0.0);
	Timer.Reset();

	EXPECT_FALSE(Timer.HasPending());
	double ElapsedMs = -1.0;
	EXPECT_FALSE(Timer.End(1.0, ElapsedMs));
}

// -- IsValidLoadTimeMs: drop (not clamp) rules for ReportMapLoad -------------

TEST(MapLoadTimer, IsValidLoadTimeMsZeroAndPositiveValid)
{
	EXPECT_TRUE(FMapLoadTimer::IsValidLoadTimeMs(0.0));
	EXPECT_TRUE(FMapLoadTimer::IsValidLoadTimeMs(1234.5));
}

TEST(MapLoadTimer, IsValidLoadTimeMsNegativeNaNInfinityInvalid)
{
	EXPECT_FALSE(FMapLoadTimer::IsValidLoadTimeMs(-0.001));
	EXPECT_FALSE(FMapLoadTimer::IsValidLoadTimeMs(kQuietNaN));
	EXPECT_FALSE(FMapLoadTimer::IsValidLoadTimeMs(kPosInf));
	EXPECT_FALSE(FMapLoadTimer::IsValidLoadTimeMs(kNegInf));
}
