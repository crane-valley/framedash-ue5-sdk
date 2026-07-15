// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashFlushPolicy.h"

namespace Framedash
{

FFlushPolicy::FFlushPolicy(
	int InMaxBatchSize,
	int InMaxPayloadBytes,
	float InFlushIntervalSeconds,
	int InBytesPerEventEstimate)
	: MaxBatchSize(InMaxBatchSize > 0 ? InMaxBatchSize : 100)
	, MaxPayloadBytes(InMaxPayloadBytes > 0 ? InMaxPayloadBytes : 102400)
	, FlushIntervalSeconds(InFlushIntervalSeconds > 0.0f ? InFlushIntervalSeconds : 30.0f)
	, BytesPerEventEstimate(InBytesPerEventEstimate > 0 ? InBytesPerEventEstimate : 500)
{
}

int FFlushPolicy::EstimatePayloadBytes(int EventCount) const
{
	if (EventCount <= 0) return 0;
	return EventCount * BytesPerEventEstimate;
}

bool FFlushPolicy::ShouldRequestFlush(int EventCount, int EstimatedBytes) const
{
	if (EventCount < 0) EventCount = 0;
	if (EstimatedBytes < 0) EstimatedBytes = 0;
	return EventCount >= MaxBatchSize || EstimatedBytes >= MaxPayloadBytes;
}

bool FFlushPolicy::ShouldFlush(bool bFlushRequested, float ElapsedSeconds) const
{
	if (ElapsedSeconds < 0.0f) ElapsedSeconds = 0.0f;
	return bFlushRequested || ElapsedSeconds >= FlushIntervalSeconds;
}

} // namespace Framedash
