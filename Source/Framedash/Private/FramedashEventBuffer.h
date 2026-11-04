// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"

class FFramedashEventBuffer
{
public:
	explicit FFramedashEventBuffer(int32 InCapacity);

	void Enqueue(const FFramedashEvent& Event);
	void Enqueue(FFramedashEvent&& Event);

	/** Add an event only when doing so will not overwrite the oldest entry. */
	bool TryEnqueuePreservingOldest(const FFramedashEvent& Event);
	bool TryEnqueuePreservingOldest(FFramedashEvent&& Event);

	TArray<FFramedashEvent> DequeueAll();

	int32 Count() const;

	bool IsEmpty() const;

private:
	TArray<FFramedashEvent> Buffer;
	int32 Capacity;
	int32 Head = 0;
	int32 Tail = 0;
	int32 CurrentCount = 0;
	mutable FCriticalSection CriticalSection;

	void AdvanceAfterWriteLocked();
};
