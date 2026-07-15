// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"

/**
 * Thread-safe ring buffer for telemetry events.
 * When full, the oldest events are dropped (game performance takes priority).
 */
class FFramedashEventBuffer
{
public:
	explicit FFramedashEventBuffer(int32 InCapacity);

	/** Add an event to the buffer. Returns false if event was dropped (buffer full and overwritten). */
	void Enqueue(const FFramedashEvent& Event);
	void Enqueue(FFramedashEvent&& Event);

	/** Remove and return all buffered events. */
	TArray<FFramedashEvent> DequeueAll();

	/** Current number of events in the buffer. */
	int32 Count() const;

	/** Whether the buffer is empty. */
	bool IsEmpty() const;

private:
	TArray<FFramedashEvent> Buffer;
	int32 Capacity;
	int32 Head = 0; // Next write position
	int32 Tail = 0; // Next read position
	int32 CurrentCount = 0;
	mutable FCriticalSection CriticalSection;
};
