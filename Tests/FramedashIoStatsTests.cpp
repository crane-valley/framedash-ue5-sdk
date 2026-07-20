// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Standalone unit tests for the pure C++ disk-IO accumulator in
// FramedashIoStats.h. No UE5 headers required -- the header is pure C++ /
// <atomic> / <cmath>. The accumulator feeds the io.* window metrics attached to
// perf_heartbeat; these tests lock in the cumulative+baseline contract
// (non-destructive snapshots, per-field window subtraction with clamping,
// multiple independent baselines observing the same process-wide counters,
// non-negative-finite input clamping, and concurrent-add summation). The
// IPlatformFile wrapper that feeds it in-engine is UE-coupled and covered
// separately (engine automation).

#include "FramedashIoStats.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using Framedash::FIoMeteringSuppressionScope;
using Framedash::FIoSnapshot;
using Framedash::FIoStatsAccumulator;
using Framedash::IsThreadReadSuppressed;

namespace
{
const double kQuietNaN = std::numeric_limits<double>::quiet_NaN();
const double kPosInf = std::numeric_limits<double>::infinity();
const double kNegInf = -std::numeric_limits<double>::infinity();
} // namespace

// -- Basic accumulation ------------------------------------------------------

TEST(IoStatsAccumulator, EmptyCumulativeIsZero)
{
	FIoStatsAccumulator Acc;
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, 0);
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, 0.0);
	EXPECT_EQ(Snapshot.ReadOps, 0);
}

TEST(IoStatsAccumulator, AddReadAndOpsAccumulateCumulatively)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(4096, 1.5);
	Acc.AddOps(1);
	Acc.AddRead(2048, 0.5);
	Acc.AddOps(1);

	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, 6144);
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, 2.0);
	EXPECT_EQ(Snapshot.ReadOps, 2);
}

TEST(IoStatsAccumulator, AddReadDoesNotCountOps)
{
	// AddRead only sums bytes/time; ops come exclusively from AddOps so a
	// zero-byte read can still be counted as an operation by the caller.
	FIoStatsAccumulator Acc;
	Acc.AddRead(1024, 1.0);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, 1024);
	EXPECT_EQ(Snapshot.ReadOps, 0);
}

// -- Non-destructive reads ---------------------------------------------------

TEST(IoStatsAccumulator, ReadCumulativeIsNonDestructive)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(1000, 2.0);
	Acc.AddOps(3);

	// Reading the cumulative totals must NOT reset them -- repeated reads with no
	// intervening adds return the identical (monotonic) values.
	const FIoSnapshot First = Acc.ReadCumulative();
	const FIoSnapshot Second = Acc.ReadCumulative();
	EXPECT_EQ(First.ReadBytes, 1000);
	EXPECT_EQ(Second.ReadBytes, 1000);
	EXPECT_DOUBLE_EQ(Second.ReadTimeMs, 2.0);
	EXPECT_EQ(Second.ReadOps, 3);
}

// -- Window via baseline (Since) ---------------------------------------------

TEST(IoStatsAccumulator, WindowIsCumulativeMinusBaseline)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(1000, 2.0);
	Acc.AddOps(3);

	// First window: baseline is the zero snapshot at construction.
	FIoSnapshot Baseline;
	FIoSnapshot Now = Acc.ReadCumulative();
	FIoSnapshot Window = Now.Since(Baseline);
	EXPECT_EQ(Window.ReadBytes, 1000);
	EXPECT_DOUBLE_EQ(Window.ReadTimeMs, 2.0);
	EXPECT_EQ(Window.ReadOps, 3);
	Baseline = Now; // consumer advances its baseline

	// No activity: next window is a clean zero (cumulative unchanged).
	Now = Acc.ReadCumulative();
	Window = Now.Since(Baseline);
	EXPECT_EQ(Window.ReadBytes, 0);
	EXPECT_DOUBLE_EQ(Window.ReadTimeMs, 0.0);
	EXPECT_EQ(Window.ReadOps, 0);
	Baseline = Now;

	// New activity accumulates only into the new window.
	Acc.AddRead(500, 1.0);
	Acc.AddOps(1);
	Now = Acc.ReadCumulative();
	Window = Now.Since(Baseline);
	EXPECT_EQ(Window.ReadBytes, 500);
	EXPECT_DOUBLE_EQ(Window.ReadTimeMs, 1.0);
	EXPECT_EQ(Window.ReadOps, 1);
}

TEST(IoStatsAccumulator, IndependentBaselinesSeeSameProcessWideIo)
{
	// Models multi-client PIE: two subsystems share ONE process-global
	// accumulator, each with its own baseline advancing on its own heartbeat.
	// Neither drains the other's window; both observe the same cumulative IO.
	FIoStatsAccumulator Acc;
	FIoSnapshot BaselineA; // subsystem A
	FIoSnapshot BaselineB; // subsystem B

	Acc.AddRead(800, 4.0);
	Acc.AddOps(2);

	// A drains first at its heartbeat.
	FIoSnapshot SnapA1 = Acc.ReadCumulative();
	FIoSnapshot WindowA1 = SnapA1.Since(BaselineA);
	BaselineA = SnapA1;
	EXPECT_EQ(WindowA1.ReadBytes, 800);
	EXPECT_EQ(WindowA1.ReadOps, 2);

	// B's heartbeat fires later but sees the SAME 800 bytes (not zero) -- the
	// core regression this contract fixes.
	FIoSnapshot SnapB1 = Acc.ReadCumulative();
	FIoSnapshot WindowB1 = SnapB1.Since(BaselineB);
	BaselineB = SnapB1;
	EXPECT_EQ(WindowB1.ReadBytes, 800);
	EXPECT_EQ(WindowB1.ReadOps, 2);
	EXPECT_DOUBLE_EQ(WindowB1.ReadTimeMs, 4.0);

	// More activity, then A drains: A sees only the new delta.
	Acc.AddRead(200, 1.0);
	Acc.AddOps(1);
	FIoSnapshot SnapA2 = Acc.ReadCumulative();
	FIoSnapshot WindowA2 = SnapA2.Since(BaselineA);
	EXPECT_EQ(WindowA2.ReadBytes, 200);
	EXPECT_EQ(WindowA2.ReadOps, 1);
}

TEST(IoSnapshot, SinceClampsBaselineRegressionToZero)
{
	// Defensive: if a baseline somehow observed a LARGER value than the current
	// snapshot (torn read of independent atomics, or a future reset path), the
	// window must clamp to 0 per field, never emit a negative metric.
	FIoSnapshot Now;
	Now.ReadBytes = 100;
	Now.ReadTimeMs = 1.0;
	Now.ReadOps = 5;

	FIoSnapshot HigherBaseline;
	HigherBaseline.ReadBytes = 1000;
	HigherBaseline.ReadTimeMs = 10.0;
	HigherBaseline.ReadOps = 50;

	const FIoSnapshot Window = Now.Since(HigherBaseline);
	EXPECT_EQ(Window.ReadBytes, 0);
	EXPECT_DOUBLE_EQ(Window.ReadTimeMs, 0.0);
	EXPECT_EQ(Window.ReadOps, 0);
}

// -- Clamping: bytes ---------------------------------------------------------

TEST(IoStatsAccumulator, NegativeBytesDropped)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(-5000, 1.0);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, 0);
	// The finite positive time on the same call is still accumulated.
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, 1.0);
}

TEST(IoStatsAccumulator, ZeroBytesAddNothing)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(0, 0.0);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, 0);
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, 0.0);
}

// -- Clamping: time ----------------------------------------------------------

TEST(IoStatsAccumulator, NaNTimeDropped)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(1024, kQuietNaN);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	// Bytes still accumulate; the NaN time is dropped, leaving a finite total.
	EXPECT_EQ(Snapshot.ReadBytes, 1024);
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, 0.0);
	EXPECT_TRUE(std::isfinite(Snapshot.ReadTimeMs));
}

TEST(IoStatsAccumulator, InfiniteTimeDropped)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(1024, kPosInf);
	Acc.AddRead(1024, kNegInf);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, 2048);
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, 0.0);
	EXPECT_TRUE(std::isfinite(Snapshot.ReadTimeMs));
}

TEST(IoStatsAccumulator, NegativeTimeDropped)
{
	FIoStatsAccumulator Acc;
	Acc.AddRead(1024, -3.0);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, 1024);
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, 0.0);
}

// -- Clamping: ops -----------------------------------------------------------

TEST(IoStatsAccumulator, NegativeOpsDropped)
{
	FIoStatsAccumulator Acc;
	Acc.AddOps(-10);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadOps, 0);
}

TEST(IoStatsAccumulator, ZeroOpsDropped)
{
	FIoStatsAccumulator Acc;
	Acc.AddOps(0);
	Acc.AddOps(4);
	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadOps, 4);
}

// The io.* heartbeat-attach gate is session-scoped state on the subsystem
// (bTrackDiskIoInstalled / bIoManualFeedAccepted), NOT a process-global latch on
// the accumulator, so a later untracked session in the same process emits no
// io.* keys. That subsystem gate is UE-coupled (verified by engine build /
// review); the accumulator here carries no active/latch notion by design.

// -- Concurrency -------------------------------------------------------------

TEST(IoStatsAccumulator, ConcurrentAddsSumCorrectly)
{
	FIoStatsAccumulator Acc;
	constexpr int kThreads = 8;
	constexpr int kIterations = 10000;

	std::vector<std::thread> Workers;
	Workers.reserve(kThreads);
	for (int T = 0; T < kThreads; ++T)
	{
		Workers.emplace_back([&Acc]()
		{
			for (int I = 0; I < kIterations; ++I)
			{
				Acc.AddRead(100, 0.25);
				Acc.AddOps(1);
			}
		});
	}
	for (auto&& Worker : Workers)
	{
		Worker.join();
	}

	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	const int64_t kExpectedOps =
		static_cast<int64_t>(kThreads) * static_cast<int64_t>(kIterations);
	EXPECT_EQ(Snapshot.ReadBytes, kExpectedOps * 100);
	EXPECT_EQ(Snapshot.ReadOps, kExpectedOps);
	EXPECT_DOUBLE_EQ(Snapshot.ReadTimeMs, static_cast<double>(kExpectedOps) * 0.25);
}

// -- Read-metering suppression scope -----------------------------------------

TEST(IoMeteringSuppression, DefaultsToNotSuppressed)
{
	EXPECT_FALSE(IsThreadReadSuppressed());
}

TEST(IoMeteringSuppression, ScopeSuppressesThenRestores)
{
	EXPECT_FALSE(IsThreadReadSuppressed());
	{
		FIoMeteringSuppressionScope Scope;
		EXPECT_TRUE(IsThreadReadSuppressed());
	}
	// Balanced on scope exit -- the SDK's own read window closes with the guard.
	EXPECT_FALSE(IsThreadReadSuppressed());
}

TEST(IoMeteringSuppression, NestsCorrectly)
{
	EXPECT_FALSE(IsThreadReadSuppressed());
	{
		FIoMeteringSuppressionScope Outer;
		EXPECT_TRUE(IsThreadReadSuppressed());
		{
			FIoMeteringSuppressionScope Inner;
			EXPECT_TRUE(IsThreadReadSuppressed());
		}
		// Still suppressed while the outer scope is live.
		EXPECT_TRUE(IsThreadReadSuppressed());
	}
	EXPECT_FALSE(IsThreadReadSuppressed());
}

TEST(IoMeteringSuppression, IsThreadLocal)
{
	// A suppression scope on THIS thread must not leak to another thread -- the
	// wrapper meters concurrent game reads on other threads normally while the SDK
	// reads its own queue.
	FIoMeteringSuppressionScope Scope;
	ASSERT_TRUE(IsThreadReadSuppressed());

	bool bOtherThreadSuppressed = true;
	std::thread Other([&bOtherThreadSuppressed]()
	{
		bOtherThreadSuppressed = IsThreadReadSuppressed();
	});
	Other.join();

	EXPECT_FALSE(bOtherThreadSuppressed);
	EXPECT_TRUE(IsThreadReadSuppressed()); // this thread still suppressed
}

// -- Saturation (overflow safety) --------------------------------------------

TEST(IoStatsAccumulator, ByteAndOpAddsSaturateAtInt64Max)
{
	// Two adds of INT64_MAX must clamp at INT64_MAX, never wrap to a negative /
	// garbage value that would break the monotonic cumulative contract.
	constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
	FIoStatsAccumulator Acc;
	Acc.AddRead(kMax, 0.0);
	Acc.AddRead(kMax, 0.0);
	Acc.AddOps(kMax);
	Acc.AddOps(kMax);

	const FIoSnapshot Snapshot = Acc.ReadCumulative();
	EXPECT_EQ(Snapshot.ReadBytes, kMax);
	EXPECT_EQ(Snapshot.ReadOps, kMax);
	EXPECT_GE(Snapshot.ReadBytes, 0);
	EXPECT_GE(Snapshot.ReadOps, 0);
}

TEST(IoStatsAccumulator, SinceAcrossSaturatedCounterStaysNonNegative)
{
	// A window computed across a saturated counter must never go negative: the
	// counter holds at MAX (monotonic), so Since(baseline) is a well-defined
	// non-negative delta rather than a signed-overflow subtraction.
	constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
	FIoStatsAccumulator Acc;

	Acc.AddRead(1000, 1.0);
	Acc.AddOps(2);
	const FIoSnapshot Baseline = Acc.ReadCumulative();

	// Saturate the counters after taking the baseline.
	Acc.AddRead(kMax, 0.0);
	Acc.AddOps(kMax);
	const FIoSnapshot Now = Acc.ReadCumulative();

	const FIoSnapshot Window = Now.Since(Baseline);
	EXPECT_GE(Window.ReadBytes, 0);
	EXPECT_GE(Window.ReadOps, 0);
	// Now is clamped at MAX, so the delta is MAX - baseline (both non-negative).
	EXPECT_EQ(Window.ReadBytes, kMax - 1000);
	EXPECT_EQ(Window.ReadOps, kMax - 2);
}
