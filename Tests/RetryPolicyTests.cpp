// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Standalone unit tests for Framedash::FRetryPolicy.
// Based on sdks/unity/Tests/RetryPolicyTests.cs, with UE5-specific
// assertions for preserved transport behavior: 3xx fail-fast and
// valid zero retry/delay configuration.

#include "FramedashRetryPolicy.h"

#include <cmath>
#include <gtest/gtest.h>

using Framedash::ERetryAction;
using Framedash::FRetryPolicy;

namespace
{

constexpr int kMaxRetries = 5;
constexpr float kBaseDelay = 1.0f;

FRetryPolicy MakeDefault()
{
	return FRetryPolicy(kMaxRetries, kBaseDelay);
}

} // namespace

// -- IsNonRetryableError --------------------------------------------------

TEST(RetryPolicy, IsNonRetryableError_4xx_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.IsNonRetryableError(400));
	EXPECT_TRUE(Policy.IsNonRetryableError(401));
	EXPECT_TRUE(Policy.IsNonRetryableError(403));
	EXPECT_TRUE(Policy.IsNonRetryableError(404));
	EXPECT_TRUE(Policy.IsNonRetryableError(422));
}

TEST(RetryPolicy, IsNonRetryableError_413_ReturnsFalse)
{
	// 413 triggers batch split, not a non-retryable error.
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.IsNonRetryableError(413));
}

TEST(RetryPolicy, IsNonRetryableError_429_ReturnsFalse)
{
	// 429 is retryable (rate limit backoff).
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.IsNonRetryableError(429));
}

TEST(RetryPolicy, IsNonRetryableError_5xx_ReturnsFalse)
{
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.IsNonRetryableError(500));
	EXPECT_FALSE(Policy.IsNonRetryableError(503));
}

// -- ShouldSplitBatch -----------------------------------------------------

TEST(RetryPolicy, ShouldSplitBatch_413_MultipleEvents_ReturnsTrue)
{
	const auto Policy = MakeDefault();
	EXPECT_TRUE(Policy.ShouldSplitBatch(413, 2));
	EXPECT_TRUE(Policy.ShouldSplitBatch(413, 100));
}

TEST(RetryPolicy, ShouldSplitBatch_413_SingleEvent_ReturnsFalse)
{
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.ShouldSplitBatch(413, 1));
	EXPECT_FALSE(Policy.ShouldSplitBatch(413, 0));
}

TEST(RetryPolicy, ShouldSplitBatch_NonOversize_ReturnsFalse)
{
	const auto Policy = MakeDefault();
	EXPECT_FALSE(Policy.ShouldSplitBatch(400, 10));
	EXPECT_FALSE(Policy.ShouldSplitBatch(500, 10));
	EXPECT_FALSE(Policy.ShouldSplitBatch(200, 10));
}

// -- GetRetryDelaySeconds -------------------------------------------------

TEST(RetryPolicy, GetRetryDelaySeconds_ExponentialBackoff)
{
	const auto Policy = MakeDefault();
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(0), 1.0f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(1), 2.0f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(2), 4.0f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(3), 8.0f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(4), 16.0f);
}

TEST(RetryPolicy, GetRetryDelaySeconds_LargeAttempt_DoesNotOverflow)
{
	const auto Policy = MakeDefault();
	const float Delay = Policy.GetRetryDelaySeconds(50);
	EXPECT_FALSE(std::isinf(Delay));
	EXPECT_FALSE(std::isnan(Delay));
	EXPECT_FLOAT_EQ(Delay, 60.0f);
}

TEST(RetryPolicy, GetRetryDelaySeconds_CustomBase_ClampsAtMaxDelay)
{
	FRetryPolicy Policy(/*MaxRetries=*/10, /*BaseDelaySeconds=*/10.0f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(3), 60.0f);
}

TEST(RetryPolicy, GetRetryDelaySeconds_NegativeAttempt_TreatsAsZero)
{
	const auto Policy = MakeDefault();
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(-1), 1.0f);
}

TEST(RetryPolicy, GetRetryDelaySeconds_CustomBase_ScalesCorrectly)
{
	FRetryPolicy Policy(/*MaxRetries=*/3, /*BaseDelaySeconds=*/0.5f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(0), 0.5f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(1), 1.0f);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(2), 2.0f);
}

// -- Classify -------------------------------------------------------------

TEST(RetryPolicy, Classify_2xx_ReturnsSuccess)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(200, 0, 10), ERetryAction::Success);
	EXPECT_EQ(Policy.Classify(204, 0, 10), ERetryAction::Success);
}

TEST(RetryPolicy, Classify_413_MultipleEvents_ReturnsSplitBatch)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(413, 0, 10), ERetryAction::SplitBatch);
}

TEST(RetryPolicy, Classify_413_SingleEvent_ReturnsFail)
{
	// Cannot split a single event; treat as non-retryable.
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(413, 0, 1), ERetryAction::Fail);
}

TEST(RetryPolicy, Classify_4xx_ReturnsFail)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(400, 0, 10), ERetryAction::Fail);
	EXPECT_EQ(Policy.Classify(401, 0, 10), ERetryAction::Fail);
}

TEST(RetryPolicy, Classify_5xx_WithAttemptsLeft_ReturnsRetry)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(500, 0, 10), ERetryAction::Retry);
	EXPECT_EQ(Policy.Classify(500, 4, 10), ERetryAction::Retry);
}

TEST(RetryPolicy, Classify_5xx_NoAttemptsLeft_ReturnsFail)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(500, 5, 10), ERetryAction::Fail);
}

TEST(RetryPolicy, Classify_429_WithAttemptsLeft_ReturnsRetry)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(429, 0, 10), ERetryAction::Retry);
}

TEST(RetryPolicy, Classify_429_NoAttemptsLeft_ReturnsFail)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(429, 5, 10), ERetryAction::Fail);
}

TEST(RetryPolicy, Classify_NetworkError_WithAttemptsLeft_ReturnsRetry)
{
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(0, 0, 10), ERetryAction::Retry);
}

TEST(RetryPolicy, Classify_3xx_ReturnsFail)
{
	// UE HTTP module follows redirects internally. A 3xx that escapes
	// usually means a misconfigured endpoint, so fail fast instead of
	// burning the whole retry budget on every batch.
	const auto Policy = MakeDefault();
	EXPECT_EQ(Policy.Classify(301, 0, 10), ERetryAction::Fail);
	EXPECT_EQ(Policy.Classify(302, 0, 10), ERetryAction::Fail);
	EXPECT_EQ(Policy.Classify(304, 0, 10), ERetryAction::Fail);
}

// -- Constructor defaults -------------------------------------------------

TEST(RetryPolicy, Constructor_DefaultValues)
{
	FRetryPolicy Policy;
	EXPECT_EQ(Policy.GetMaxRetries(), 5);
	EXPECT_FLOAT_EQ(Policy.GetBaseDelaySeconds(), 1.0f);
}

TEST(RetryPolicy, Constructor_InvalidValues_FallsBackToDefaults)
{
	FRetryPolicy Policy(/*MaxRetries=*/-1, /*BaseDelaySeconds=*/-1.0f);
	EXPECT_EQ(Policy.GetMaxRetries(), 5);
	EXPECT_FLOAT_EQ(Policy.GetBaseDelaySeconds(), 1.0f);
}

TEST(RetryPolicy, Constructor_ZeroValues_AreAllowed)
{
	FRetryPolicy Policy(/*MaxRetries=*/0, /*BaseDelaySeconds=*/0.0f);
	EXPECT_EQ(Policy.GetMaxRetries(), 0);
	EXPECT_FLOAT_EQ(Policy.GetBaseDelaySeconds(), 0.0f);
	EXPECT_EQ(Policy.Classify(500, 0, 10), ERetryAction::Fail);
	EXPECT_FLOAT_EQ(Policy.GetRetryDelaySeconds(0), 0.0f);
}

TEST(RetryPolicy, Constructor_CustomValues)
{
	FRetryPolicy Policy(/*MaxRetries=*/3, /*BaseDelaySeconds=*/2.0f);
	EXPECT_EQ(Policy.GetMaxRetries(), 3);
	EXPECT_FLOAT_EQ(Policy.GetBaseDelaySeconds(), 2.0f);
}
