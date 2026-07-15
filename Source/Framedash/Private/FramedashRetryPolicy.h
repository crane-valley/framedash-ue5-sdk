// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

// Engine-independent: this header MUST NOT include UE5 headers
// (CoreMinimal.h, Containers/*, etc.). Pure C++ so it can be
// compiled into the standalone GoogleTest harness under sdks/ue5/Tests
// without an UnrealEditor build.

namespace Framedash
{

enum class ERetryAction
{
	Success,
	Retry,
	SplitBatch,
	Fail,
};

// Pure retry decision logic extracted from FFramedashTransport.
// Mirrors the Unity SDK RetryPolicy shape while preserving UE5 transport
// behavior where it intentionally differs (3xx fail-fast, zero values allowed).
class FRetryPolicy
{
public:
	explicit FRetryPolicy(int InMaxRetries = 5, float InBaseDelaySeconds = 1.0f);

	int GetMaxRetries() const { return MaxRetries; }
	float GetBaseDelaySeconds() const { return BaseDelaySeconds; }

	// True when the response warrants splitting the batch in half.
	// Only HTTP 413 with more than one event qualifies; a single
	// oversized event cannot be split further.
	bool ShouldSplitBatch(int HttpStatusCode, int EventCount) const;

	// True for 4xx responses that should not be retried.
	// 413 (split) and 429 (rate limit) are deliberately excluded.
	bool IsNonRetryableError(int HttpStatusCode) const;

	// Exponential backoff: BaseDelaySeconds * 2^Attempt.
	// Negative attempts clamp to 0.
	float GetRetryDelaySeconds(int Attempt) const;

	// Classify the response into the action the transport should take.
	// 5xx, 429, and network errors (status 0) are retried until MaxRetries
	// is reached. A surfaced 3xx is treated as Fail: Unity sets
	// redirectLimit=0 so a redirect surfaces here directly, while UE's curl
	// backend follows redirects (no portable toggle to disable) so a 3xx
	// rarely reaches this point and the UE transport instead detects a
	// cross-origin landing via the response's effective URL. Either way a
	// surfaced 3xx means a misconfigured or compromised endpoint that should
	// fail fast rather than consume the retry budget. Unity matches this.
	ERetryAction Classify(int HttpStatusCode, int Attempt, int EventCount) const;

private:
	int MaxRetries;
	float BaseDelaySeconds;
};

} // namespace Framedash
