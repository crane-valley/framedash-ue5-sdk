// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

// Engine-independent: this header MUST NOT include UE5 headers
// (CoreMinimal.h, Containers/*, etc.). Pure C++ so it can be
// compiled into the standalone GoogleTest harness under sdks/ue5/Tests
// without an UnrealEditor build.

namespace Framedash
{

// Pure flush decision logic extracted from UFramedashSubsystem.
// Mirrors the Unity SDK FlushPolicy shape so both SDKs share the
// same trigger contract: 100 events OR 30s interval OR 100KB
// estimated payload (whichever first).
class FFlushPolicy
{
public:
	explicit FFlushPolicy(
		int InMaxBatchSize = 100,
		int InMaxPayloadBytes = 102400,
		float InFlushIntervalSeconds = 30.0f,
		int InBytesPerEventEstimate = 500);

	int GetMaxBatchSize() const { return MaxBatchSize; }
	int GetMaxPayloadBytes() const { return MaxPayloadBytes; }
	float GetFlushIntervalSeconds() const { return FlushIntervalSeconds; }
	int GetBytesPerEventEstimate() const { return BytesPerEventEstimate; }

	// Estimated payload size in bytes for the given event count.
	int EstimatePayloadBytes(int EventCount) const;

	// True when the buffer-side trigger fires after Track(): event count
	// reaches the batch limit or the running estimated payload reaches
	// the byte limit. Negative inputs are treated as zero.
	bool ShouldRequestFlush(int EventCount, int EstimatedBytes) const;

	// True when the periodic tick should call Flush(): either an explicit
	// flush request is pending or the configured interval has elapsed.
	// Negative ElapsedSeconds is treated as zero.
	bool ShouldFlush(bool bFlushRequested, float ElapsedSeconds) const;

private:
	int MaxBatchSize;
	int MaxPayloadBytes;
	float FlushIntervalSeconds;
	int BytesPerEventEstimate;
};

} // namespace Framedash
