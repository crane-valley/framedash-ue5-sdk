// Copyright Crane Valley. All Rights Reserved.

#include "FramedashEventBuffer.h"

FFramedashEventBuffer::FFramedashEventBuffer(int32 InCapacity)
	: Capacity(FMath::Max(InCapacity, 1))
{
	Buffer.SetNum(Capacity);
}

void FFramedashEventBuffer::Enqueue(const FFramedashEvent& Event)
{
	FScopeLock Lock(&CriticalSection);

	Buffer[Head] = Event;
	Head = (Head + 1) % Capacity;

	if (CurrentCount == Capacity)
	{
		// Buffer full - overwrite oldest, advance tail
		Tail = (Tail + 1) % Capacity;
	}
	else
	{
		++CurrentCount;
	}
}

void FFramedashEventBuffer::Enqueue(FFramedashEvent&& Event)
{
	FScopeLock Lock(&CriticalSection);

	Buffer[Head] = MoveTemp(Event);
	Head = (Head + 1) % Capacity;

	if (CurrentCount == Capacity)
	{
		Tail = (Tail + 1) % Capacity;
	}
	else
	{
		++CurrentCount;
	}
}

TArray<FFramedashEvent> FFramedashEventBuffer::DequeueAll()
{
	TArray<FFramedashEvent> Snapshot;
	int32 Count_Local;
	int32 Tail_Local;

	// Pre-allocate replacement buffer outside the lock so that
	// the critical section is O(1) (pointer swaps only).
	TArray<FFramedashEvent> NewBuffer;
	NewBuffer.SetNum(Capacity);

	{
		FScopeLock Lock(&CriticalSection);

		Count_Local = CurrentCount;
		if (Count_Local == 0)
		{
			return TArray<FFramedashEvent>();
		}

		Tail_Local = Tail;
		Swap(Buffer, Snapshot);
		Buffer = MoveTemp(NewBuffer);
		Head = 0;
		Tail = 0;
		CurrentCount = 0;
	}

	// Extract events outside the lock — game thread Enqueue() is no longer blocked
	TArray<FFramedashEvent> Result;
	Result.Reserve(Count_Local);

	for (int32 i = 0; i < Count_Local; ++i)
	{
		const int32 Index = (Tail_Local + i) % Capacity;
		Result.Add(MoveTemp(Snapshot[Index]));
	}

	return Result;
}

int32 FFramedashEventBuffer::Count() const
{
	FScopeLock Lock(&CriticalSection);
	return CurrentCount;
}

bool FFramedashEventBuffer::IsEmpty() const
{
	FScopeLock Lock(&CriticalSection);
	return CurrentCount == 0;
}
