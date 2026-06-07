// Copyright Crane Valley. All Rights Reserved.

#include "FramedashRetryPolicy.h"

#include <cmath>

namespace Framedash
{

namespace
{

constexpr float MaxRetryDelaySeconds = 60.0f;

} // anonymous namespace

FRetryPolicy::FRetryPolicy(int InMaxRetries, float InBaseDelaySeconds)
	: MaxRetries(InMaxRetries >= 0 ? InMaxRetries : 5)
	, BaseDelaySeconds(InBaseDelaySeconds >= 0.0f ? InBaseDelaySeconds : 1.0f)
{
}

bool FRetryPolicy::ShouldSplitBatch(int HttpStatusCode, int EventCount) const
{
	return HttpStatusCode == 413 && EventCount > 1;
}

bool FRetryPolicy::IsNonRetryableError(int HttpStatusCode) const
{
	return HttpStatusCode >= 400 && HttpStatusCode < 500
		&& HttpStatusCode != 413
		&& HttpStatusCode != 429;
}

float FRetryPolicy::GetRetryDelaySeconds(int Attempt) const
{
	if (Attempt < 0) Attempt = 0;
	if (BaseDelaySeconds == 0.0f) return 0.0f;

	const float Delay = BaseDelaySeconds * std::pow(2.0f, static_cast<float>(Attempt));
	if (!std::isfinite(Delay) || Delay > MaxRetryDelaySeconds)
	{
		return MaxRetryDelaySeconds;
	}

	return Delay;
}

ERetryAction FRetryPolicy::Classify(int HttpStatusCode, int Attempt, int EventCount) const
{
	if (HttpStatusCode >= 200 && HttpStatusCode < 300)
		return ERetryAction::Success;

	if (ShouldSplitBatch(HttpStatusCode, EventCount))
		return ERetryAction::SplitBatch;

	if (IsNonRetryableError(HttpStatusCode))
		return ERetryAction::Fail;

	// 413 with a single (unsplittable) event: cannot split, cannot retry.
	if (HttpStatusCode == 413)
		return ERetryAction::Fail;

	// 3xx escapes the HTTP client only when redirect-following is disabled
	// or the redirect target is unreachable. Treat as Fail so a misconfigured
	// endpoint surfaces immediately instead of consuming the full retry budget
	// on every batch (matches the original UE5 transport behavior; this
	// deliberately diverges from the Unity RetryPolicy until Unity is
	// updated to do the same).
	if (HttpStatusCode >= 300 && HttpStatusCode < 400)
		return ERetryAction::Fail;

	// Everything else (5xx, 429, network errors with status 0, 1xx)
	// retries until the attempt budget is exhausted.
	if (Attempt >= MaxRetries)
		return ERetryAction::Fail;

	return ERetryAction::Retry;
}

} // namespace Framedash
