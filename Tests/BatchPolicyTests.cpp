// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashBatchPolicy.h"

#include <gtest/gtest.h>

using namespace Framedash;


static int64_t DecodedEntries(int32_t eventCount, int32_t attrsPerEvent = 0, int32_t metricsPerEvent = 0)
{
	return static_cast<int64_t>(eventCount)
		+ static_cast<int64_t>(eventCount) * attrsPerEvent
		+ static_cast<int64_t>(eventCount) * metricsPerEvent;
}


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


TEST(BatchPolicyTests, DoesNotSplitAtOrBelowEventCap)
{
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(
		FBatchPolicy::MaxEventsPerBatch,
		DecodedEntries(FBatchPolicy::MaxEventsPerBatch)));
}


TEST(BatchPolicyTests, SplitsAboveEventCap)
{
	EXPECT_TRUE(FBatchPolicy::ExceedsWireCaps(
		FBatchPolicy::MaxEventsPerBatch + 1,
		DecodedEntries(FBatchPolicy::MaxEventsPerBatch + 1)));
}


TEST(BatchPolicyTests, SplitsWhenDecodedEntriesExceedCapEvenBelowEventCap)
{
	// 10,000 events (== event cap, no event-count split) each with 10 attrs:
	// 10,000 + 100,000 = 110,000 decoded entries > MaxDecodedEntries (100,000).
	// The decoded-entry cap must trigger the split even though every per-event
	// count and the event count are in range.
	const int32_t eventCount = FBatchPolicy::MaxEventsPerBatch;
	const int32_t attrsPerEvent = 10;
	const int64_t decoded = DecodedEntries(eventCount, attrsPerEvent);

	EXPECT_LE(eventCount, FBatchPolicy::MaxEventsPerBatch);
	EXPECT_GT(decoded, static_cast<int64_t>(FBatchPolicy::MaxDecodedEntries));
	EXPECT_TRUE(FBatchPolicy::ExceedsWireCaps(eventCount, decoded));
}


TEST(BatchPolicyTests, HalvingABatchBringsDecodedEntriesUnderCap)
{
	// The recursive SplitBatchAndResend halves; one split must take the
	// 110,000-entry batch under the cap so it converges, not loops.
	const int32_t halfCount = FBatchPolicy::MaxEventsPerBatch / 2;
	const int64_t decoded = DecodedEntries(halfCount,  10);
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(halfCount, decoded));
}


TEST(BatchPolicyTests, CountDecodedEntries)
{
	const int64_t decoded = DecodedEntries(3,  2,  1);
	EXPECT_EQ(decoded, 12);
}


TEST(BatchPolicyTests, NeverSplitsTrivialBatches)
{
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(0, 0));
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(1, 0));
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(1, 200000LL));
	EXPECT_FALSE(FBatchPolicy::ExceedsWireCaps(-1, -1));
}

TEST(BatchPolicyTests, DoesNotSplitAtOrBelowBlockingChunkCap)
{
	EXPECT_FALSE(FBatchPolicy::ExceedsBlockingChunkBytes(
		2,
		FBatchPolicy::MaxBlockingChunkBytes - 1));
	EXPECT_FALSE(FBatchPolicy::ExceedsBlockingChunkBytes(
		2,
		FBatchPolicy::MaxBlockingChunkBytes));
}

TEST(BatchPolicyTests, SplitsAboveBlockingChunkCap)
{
	EXPECT_TRUE(FBatchPolicy::ExceedsBlockingChunkBytes(
		2,
		FBatchPolicy::MaxBlockingChunkBytes + 1));
}

TEST(BatchPolicyTests, NeverSplitsSingleEventForBlockingChunkCap)
{
	EXPECT_FALSE(FBatchPolicy::ExceedsBlockingChunkBytes(
		0,
		FBatchPolicy::MaxBlockingChunkBytes + 1));
	EXPECT_FALSE(FBatchPolicy::ExceedsBlockingChunkBytes(
		1,
		FBatchPolicy::MaxBlockingChunkBytes + 1));
}
