// Copyright Crane Valley. All Rights Reserved.

#include "FramedashBatchPolicy.h"

#include <gtest/gtest.h>

using namespace Framedash;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Count decoded entries for N events each with attrsPerEvent + metricsPerEvent
// map entries. Mirrors BatchPolicy.CountDecodedEntries semantics:
//   total = eventCount + eventCount*attrsPerEvent + eventCount*metricsPerEvent
static int64_t DecodedEntries(int32_t eventCount, int32_t attrsPerEvent = 0, int32_t metricsPerEvent = 0)
{
	return static_cast<int64_t>(eventCount)
		+ static_cast<int64_t>(eventCount) * attrsPerEvent
		+ static_cast<int64_t>(eventCount) * metricsPerEvent;
}

// ---------------------------------------------------------------------------
// DoesNotSplitAtFlushBatchThreshold
// ---------------------------------------------------------------------------

TEST(BatchPolicyTests, DoesNotSplitAtFlushBatchThreshold)
{
	// The split must key off the server wire caps, not the per-flush batch
	// size. A plain drain well below the wire caps must not split.
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(100, DecodedEntries(100)));
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(
		FBatchPolicy::MaxEventsPerBatch / 2,
		DecodedEntries(FBatchPolicy::MaxEventsPerBatch / 2)));
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(
		FBatchPolicy::MaxEventsPerBatch - 1,
		DecodedEntries(FBatchPolicy::MaxEventsPerBatch - 1)));
}

// ---------------------------------------------------------------------------
// DoesNotSplitAtOrBelowEventCap
// ---------------------------------------------------------------------------

TEST(BatchPolicyTests, DoesNotSplitAtOrBelowEventCap)
{
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(
		FBatchPolicy::MaxEventsPerBatch,
		DecodedEntries(FBatchPolicy::MaxEventsPerBatch)));
}

// ---------------------------------------------------------------------------
// SplitsAboveEventCap
// ---------------------------------------------------------------------------

TEST(BatchPolicyTests, SplitsAboveEventCap)
{
	EXPECT_TRUE(FBatchPolicy::ExceedsWireCaps(
		FBatchPolicy::MaxEventsPerBatch + 1,
		DecodedEntries(FBatchPolicy::MaxEventsPerBatch + 1)));
}

// ---------------------------------------------------------------------------
// SplitsWhenDecodedEntriesExceedCapEvenBelowEventCap
// ---------------------------------------------------------------------------

TEST(BatchPolicyTests, SplitsWhenDecodedEntriesExceedCapEvenBelowEventCap)
{
	// 10,000 events (== event cap, no event-count split) each with 10 attrs:
	// 10,000 + 100,000 = 110,000 decoded entries > MaxDecodedEntries (100,000).
	// The decoded-entry cap must trigger the split even though every per-event
	// count and the event count are in range.
	const int32_t eventCount = FBatchPolicy::MaxEventsPerBatch; // 10000
	const int32_t attrsPerEvent = 10;
	const int64_t decoded = DecodedEntries(eventCount, attrsPerEvent);

	EXPECT_LE(eventCount, FBatchPolicy::MaxEventsPerBatch);
	EXPECT_GT(decoded, static_cast<int64_t>(FBatchPolicy::MaxDecodedEntries));
	EXPECT_TRUE(FBatchPolicy::ExceedsWireCaps(eventCount, decoded));
}

// ---------------------------------------------------------------------------
// HalvingABatchBringsDecodedEntriesUnderCap
// ---------------------------------------------------------------------------

TEST(BatchPolicyTests, HalvingABatchBringsDecodedEntriesUnderCap)
{
	// The recursive SplitBatchAndResend halves; one split must take the
	// 110,000-entry batch under the cap so it converges, not loops.
	const int32_t halfCount = FBatchPolicy::MaxEventsPerBatch / 2; // 5000
	const int64_t decoded = DecodedEntries(halfCount, /*attrsPerEvent=*/10);
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(halfCount, decoded));
}

// ---------------------------------------------------------------------------
// CountDecodedEntries
// ---------------------------------------------------------------------------

TEST(BatchPolicyTests, CountDecodedEntries)
{
	// 3 events, 2 attrs + 1 metric each => 3 + 6 + 3 = 12
	const int64_t decoded = DecodedEntries(3, /*attrsPerEvent=*/2, /*metricsPerEvent=*/1);
	EXPECT_EQ(decoded, 12);
}

// ---------------------------------------------------------------------------
// NeverSplitsTrivialBatches
// ---------------------------------------------------------------------------

TEST(BatchPolicyTests, NeverSplitsTrivialBatches)
{
	// A single event (or empty) is never split.
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(0, 0));
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(1, 0));
	// A single event with many map entries: event-count <= 1 => no split
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(1, 200000LL));
	// Negative counts treated as empty
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(-1, -1));
}
