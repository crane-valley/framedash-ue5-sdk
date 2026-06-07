// Copyright Crane Valley. All Rights Reserved.
//
// Standalone unit tests for Framedash::FFlushPolicy.
// Mirrors sdks/unity/Tests/FlushPolicyTests.cs so both SDKs share the
// same flush-trigger contract: 100 events OR 30s OR 100KB estimated.

#include "FramedashFlushPolicy.h"

#include <gtest/gtest.h>

using Framedash::FFlushPolicy;

namespace
{

FFlushPolicy MakeDefault()
{
	return FFlushPolicy(
		/*MaxBatchSize=*/100,
		/*MaxPayloadBytes=*/102400,
		/*FlushIntervalSeconds=*/30.0f,
		/*BytesPerEventEstimate=*/500);
}

} // namespace

// -- EstimatePayloadBytes -------------------------------------------------

TEST(FlushPolicy, EstimatePayloadBytes_MultiplyByEstimate)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.EstimatePayloadBytes(0), 0);
	EXPECT_EQ(Policy.EstimatePayloadBytes(1), 500);
	EXPECT_EQ(Policy.EstimatePayloadBytes(100), 50000);
	EXPECT_EQ(Policy.EstimatePayloadBytes(205), 102500);
}

TEST(FlushPolicy, EstimatePayloadBytes_NegativeCount_ClampsToZero)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.EstimatePayloadBytes(-1), 0);
	EXPECT_EQ(Policy.EstimatePayloadBytes(-1000), 0);
}

// -- ShouldRequestFlush ---------------------------------------------------

TEST(FlushPolicy, ShouldRequestFlush_EventCountReachesBatchSize_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldRequestFlush(100, 0));
	EXPECT_TRUE(Policy.ShouldRequestFlush(200, 0));
}

TEST(FlushPolicy, ShouldRequestFlush_EstimatedBytesReachesLimit_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldRequestFlush(1, 102400));
	EXPECT_TRUE(Policy.ShouldRequestFlush(1, 200000));
}

TEST(FlushPolicy, ShouldRequestFlush_BelowBothThresholds_ReturnsFalse)
{
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.ShouldRequestFlush(99, 50000));
	EXPECT_FALSE(Policy.ShouldRequestFlush(0, 0));
	EXPECT_FALSE(Policy.ShouldRequestFlush(50, 51200));
}

TEST(FlushPolicy, ShouldRequestFlush_ExactlyAtBatchSize_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldRequestFlush(100, 49999));
}

TEST(FlushPolicy, ShouldRequestFlush_ExactlyAtPayloadLimit_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldRequestFlush(99, 102400));
}

TEST(FlushPolicy, ShouldRequestFlush_NegativeInputs_TreatedAsZero)
{
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.ShouldRequestFlush(-1, 0));
	EXPECT_FALSE(Policy.ShouldRequestFlush(0, -1));
	EXPECT_FALSE(Policy.ShouldRequestFlush(-1000, -1000));
	// Threshold still triggers when one side is at the limit.
	EXPECT_TRUE(Policy.ShouldRequestFlush(100, -1));
	EXPECT_TRUE(Policy.ShouldRequestFlush(-1, 102400));
}

// -- ShouldFlush ----------------------------------------------------------

TEST(FlushPolicy, ShouldFlush_FlushRequested_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldFlush(true, 0.0f));
	EXPECT_TRUE(Policy.ShouldFlush(true, 29.0f));
}

TEST(FlushPolicy, ShouldFlush_IntervalElapsed_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldFlush(false, 30.0f));
	EXPECT_TRUE(Policy.ShouldFlush(false, 45.0f));
}

TEST(FlushPolicy, ShouldFlush_NeitherCondition_ReturnsFalse)
{
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.ShouldFlush(false, 0.0f));
	EXPECT_FALSE(Policy.ShouldFlush(false, 29.9f));
}

TEST(FlushPolicy, ShouldFlush_BothConditions_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldFlush(true, 30.0f));
}

TEST(FlushPolicy, ShouldFlush_NegativeElapsed_TreatsAsZero)
{
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.ShouldFlush(false, -10.0f));
	EXPECT_TRUE(Policy.ShouldFlush(true, -10.0f));
}

// -- Constructor defaults -------------------------------------------------

TEST(FlushPolicy, Constructor_DefaultValues)
{
	FFlushPolicy Policy;
	EXPECT_EQ(Policy.GetMaxBatchSize(), 100);
	EXPECT_EQ(Policy.GetMaxPayloadBytes(), 102400);
	EXPECT_FLOAT_EQ(Policy.GetFlushIntervalSeconds(), 30.0f);
	EXPECT_EQ(Policy.GetBytesPerEventEstimate(), 500);
}

TEST(FlushPolicy, Constructor_InvalidValues_FallsBackToDefaults)
{
	FFlushPolicy Policy(
		/*MaxBatchSize=*/0,
		/*MaxPayloadBytes=*/-1,
		/*FlushIntervalSeconds=*/0.0f,
		/*BytesPerEventEstimate=*/-100);
	EXPECT_EQ(Policy.GetMaxBatchSize(), 100);
	EXPECT_EQ(Policy.GetMaxPayloadBytes(), 102400);
	EXPECT_FLOAT_EQ(Policy.GetFlushIntervalSeconds(), 30.0f);
	EXPECT_EQ(Policy.GetBytesPerEventEstimate(), 500);
}

TEST(FlushPolicy, Constructor_CustomValues)
{
	FFlushPolicy Policy(
		/*MaxBatchSize=*/50,
		/*MaxPayloadBytes=*/51200,
		/*FlushIntervalSeconds=*/15.0f,
		/*BytesPerEventEstimate=*/250);
	EXPECT_EQ(Policy.GetMaxBatchSize(), 50);
	EXPECT_EQ(Policy.GetMaxPayloadBytes(), 51200);
	EXPECT_FLOAT_EQ(Policy.GetFlushIntervalSeconds(), 15.0f);
	EXPECT_EQ(Policy.GetBytesPerEventEstimate(), 250);
}

// -- Edge cases -----------------------------------------------------------

TEST(FlushPolicy, ShouldRequestFlush_SingleLargeEvent_TriggersOnBytes)
{
	FFlushPolicy Policy(
		/*MaxBatchSize=*/1000,
		/*MaxPayloadBytes=*/10,
		/*FlushIntervalSeconds=*/30.0f,
		/*BytesPerEventEstimate=*/1);
	EXPECT_TRUE(Policy.ShouldRequestFlush(10, 10));
	EXPECT_FALSE(Policy.ShouldRequestFlush(9, 9));
}

TEST(FlushPolicy, ShouldFlush_CustomInterval_RespectsValue)
{
	FFlushPolicy Policy(
		/*MaxBatchSize=*/100,
		/*MaxPayloadBytes=*/102400,
		/*FlushIntervalSeconds=*/5.0f,
		/*BytesPerEventEstimate=*/500);
	EXPECT_FALSE(Policy.ShouldFlush(false, 4.9f));
	EXPECT_TRUE(Policy.ShouldFlush(false, 5.0f));
}
