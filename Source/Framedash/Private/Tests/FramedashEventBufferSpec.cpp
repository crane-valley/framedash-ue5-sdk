// Copyright 2026 Crane Valley. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS

#include "Misc/AutomationTest.h"

#include "FramedashEventBuffer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFramedashEventBufferPersistedPrefixSpec,
	"Framedash.EventBuffer.PersistedPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFramedashEventBufferPersistedPrefixSpec::RunTest(const FString& Parameters)
{
	FFramedashEventBuffer Buffer(3);

	FFramedashEvent PersistedA;
	PersistedA.EventName = TEXT("persisted-a");
	FFramedashEvent PersistedB;
	PersistedB.EventName = TEXT("persisted-b");
	FFramedashEvent FreshC;
	FreshC.EventName = TEXT("fresh-c");
	FFramedashEvent FreshD;
	FreshD.EventName = TEXT("fresh-d");

	Buffer.Enqueue(MoveTemp(PersistedA));
	Buffer.Enqueue(MoveTemp(PersistedB));
	Buffer.Enqueue(MoveTemp(FreshC));

	TestFalse(TEXT("incoming event rejected while the oldest prefix is protected"),
		Buffer.TryEnqueuePreservingOldest(MoveTemp(FreshD)));

	const TArray<FFramedashEvent> Remaining = Buffer.DequeueAll();
	TestEqual(TEXT("buffer remains at capacity"), Remaining.Num(), 3);
	if (Remaining.Num() == 3)
	{
		TestEqual(TEXT("first persisted event retained"), Remaining[0].EventName, FString(TEXT("persisted-a")));
		TestEqual(TEXT("second persisted event retained"), Remaining[1].EventName, FString(TEXT("persisted-b")));
		TestEqual(TEXT("fresh tail retained"), Remaining[2].EventName, FString(TEXT("fresh-c")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
