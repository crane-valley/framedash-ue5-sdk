// Copyright 2026 Crane Valley. All Rights Reserved.

#include "CoreMinimal.h"

// Dev-only AND opt-in, identical gate to FramedashShutdownSpec.cpp: even under
// WITH_DEV_AUTOMATION_TESTS, BuildPlugin emits Development/Editor DLLs, so without
// FRAMEDASH_WITH_AUTOMATION_SPECS (set only by Framedash.Build.cs when
// FRAMEDASH_BUILD_AUTOMATION_SPECS=1) this destructive spec would ship inside the
// redistributable binaries and could wipe a game's persisted offline queue.
#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"

#include "FramedashSubsystem.h"
#include "FramedashSettings.h"
#include "FramedashPersistenceProvider.h"
#include "FramedashTypes.h"

// In-engine coverage for the UFramedashSubsystem::FlushBlocking contract. The fast-fail
// branches (non-positive budget, not initialized) and the timeout-keeps-events guarantee
// are all UE-type-coupled (subsystem lifecycle, isolated HTTP completion delivery,
// file-backed offline queue), so they live here rather than the engine-independent
// GoogleTest harness. A dead-loopback request can also prove that FlushBlocking does not
// drain the process-global HTTP manager and re-enter an unrelated request's callback,
// and test-only counters prove that a pre-existing async flush crosses the isolated
// background-dispatch and completion queues without draining the game task graph.
//
// The spec harness has no mock/loopback HTTP server, and a dead loopback port yields a
// transport-level status 0 rather than a delivered 2xx or a permanent 4xx. The pump can
// apply that completion, but the resulting retry dispatch is scheduled on the core ticker
// and is not driven synchronously. So the paths that need a real server response --
// success-acks-prefix, a late permanent rejection closing a queue-off batch without
// retry, and the queue-off re-enqueue on retry exhaustion -- are not deterministically
// reachable here; they are covered by logic review + the CI compile.
// The same ticker limitation prevents a deterministic public-API test of stopping a
// restored-prefix multi-wave drain after its first wave reports failure, and of the
// strict-budget verdict (a delivery confirmed past the deadline is acknowledged but
// reports false) -- producing a late 2xx needs a live server with a controllable delay.
// The delivery bookkeeping FlushBlocking reuses is exercised by FramedashShutdownSpec and
// the event-buffer/session-manager specs.
//
// The <=1 MiB estimated blocking chunk cap and the deadline checks around serialize/gzip
// are timing properties this harness cannot assert deterministically. The attribute-heavy
// case instead forces that split threshold and asserts NO-LOSS; the wall-clock bound rests
// on the pure policy tests, logic review, and CI compile.

namespace
{
	int32 CountFlushBlockingEventsByName(
		const TArray<FFramedashEvent>& Events,
		const TCHAR* Name)
	{
		int32 Total = 0;
		for (const auto& Event : Events)
		{
			if (Event.EventName == Name)
			{
				++Total;
			}
		}
		return Total;
	}
}

BEGIN_DEFINE_SPEC(FFramedashFlushBlockingSpec, "Framedash.FlushBlocking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	bool bSavedAutoInitialize = false;
	bool bSavedEnableOfflineQueue = true;
	bool bSavedCaptureCameraRotation = true;
	bool bSavedTrackDiskIo = false;

	// UFramedashSubsystem is a UGameInstanceSubsystem, so it needs a UGameInstance outer
	// (a transient-package outer trips a handled ensure). A bare GameInstance is a
	// sufficient owner: FlushBlocking never dereferences the owning GameInstance.
	UFramedashSubsystem* MakeSubsystem()
	{
		UGameInstance* Owner = NewObject<UGameInstance>(GEngine);
		return NewObject<UFramedashSubsystem>(Owner);
	}

END_DEFINE_SPEC(FFramedashFlushBlockingSpec)

void FFramedashFlushBlockingSpec::Define()
{
	BeforeEach([this]()
	{
		UFramedashSettings* Settings = GetMutableDefault<UFramedashSettings>();
		bSavedAutoInitialize = Settings->bAutoInitialize;
		bSavedEnableOfflineQueue = Settings->bEnableOfflineQueue;
		bSavedCaptureCameraRotation = Settings->bCaptureCameraRotation;
		bSavedTrackDiskIo = Settings->bTrackDiskIo;

		// A detached NewObject subsystem never runs the collection auto-init; pin these
		// off so no other path perturbs the test. Camera capture is off because a detached
		// subsystem has no PlayerController to sample.
		Settings->bAutoInitialize = false;
		Settings->bCaptureCameraRotation = false;
		Settings->bTrackDiskIo = false;

		FFilePersistence().Clear();
	});

	AfterEach([this]()
	{
		FFilePersistence().Clear();

		UFramedashSettings* Settings = GetMutableDefault<UFramedashSettings>();
		Settings->bAutoInitialize = bSavedAutoInitialize;
		Settings->bEnableOfflineQueue = bSavedEnableOfflineQueue;
		Settings->bCaptureCameraRotation = bSavedCaptureCameraRotation;
		Settings->bTrackDiskIo = bSavedTrackDiskIo;
	});

	It("returns false without blocking when the SDK is not initialized", [this]()
	{
		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();

		TestFalse(TEXT("FlushBlocking is false when uninitialized"), Subsystem->FlushBlocking(1000));

		Subsystem->RemoveFromRoot();
	});

	It("returns false for a non-positive timeout", [this]()
	{
		GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();
		// Loopback http passes the endpoint-security check, so the only reason to reject
		// here is the non-positive budget.
		Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
		TestTrue(TEXT("subsystem initialized"), Subsystem->IsInitialized());

		TestFalse(TEXT("zero timeout is rejected"), Subsystem->FlushBlocking(0));
		TestFalse(TEXT("negative timeout is rejected"), Subsystem->FlushBlocking(-100));
		TestTrue(TEXT("still initialized after rejected calls"), Subsystem->IsInitialized());

		Subsystem->Deinitialize();
		Subsystem->RemoveFromRoot();
	});

	It("times out without delivery, returns false, and keeps events queued", [this]()
	{
		GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();

		// Port 1 on loopback is unroutable, so the HTTP POST can never complete within the
		// budget. A short budget bounds the pump; the flush stays in flight at the deadline,
		// so FlushBlocking must report failure without losing any events.
		Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
		Subsystem->Track(TEXT("blocking_event"));

		const bool bDelivered = Subsystem->FlushBlocking(100);
		TestFalse(TEXT("undeliverable flush times out to false"), bDelivered);
		TestTrue(TEXT("still initialized after a timeout"), Subsystem->IsInitialized());

		Subsystem->Deinitialize();
		const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
		TestEqual(TEXT("blocking_event persisted after timeout"),
			CountFlushBlockingEventsByName(Persisted, TEXT("blocking_event")), 1);
		TestEqual(TEXT("session_start persisted after timeout"),
			CountFlushBlockingEventsByName(Persisted, TEXT("session_start")), 1);

		Subsystem->RemoveFromRoot();
	});

	It("does not drain unrelated HTTP completions", [this]()
	{
		GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

		// The default policy queues this callback for the process-global HTTP manager.
		// Sleeping gives the background HTTP thread time to finish without advancing that
		// shared queue. FlushBlocking must not fire arbitrary game-owned delegates while
		// the caller is inside a supposedly Framedash-only blocking wait.
		TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> ThirdPartyCompleted =
			MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> ThirdPartyRequest =
			FHttpModule::Get().CreateRequest();
		ThirdPartyRequest->SetURL(TEXT("http://127.0.0.1:1"));
		ThirdPartyRequest->SetVerb(TEXT("GET"));
		ThirdPartyRequest->SetTimeout(0.05f);
		ThirdPartyRequest->OnProcessRequestComplete().BindLambda(
			[ThirdPartyCompleted](FHttpRequestPtr, FHttpResponsePtr, bool)
			{
				ThirdPartyCompleted->store(true, std::memory_order_release);
			});
		TestTrue(TEXT("unrelated request dispatched"), ThirdPartyRequest->ProcessRequest());
		FPlatformProcess::Sleep(0.1f);
		TestFalse(TEXT("unrelated completion remains queued before flush"),
			ThirdPartyCompleted->load(std::memory_order_acquire));

		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();
		Subsystem->InitializeTelemetry(
			TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
		Subsystem->Track(TEXT("isolated_wait_event"));

		TestFalse(TEXT("dead loopback still times out"), Subsystem->FlushBlocking(100));
		TestFalse(TEXT("blocking wait does not fire unrelated completion"),
			ThirdPartyCompleted->load(std::memory_order_acquire));

		Subsystem->Deinitialize();
		Subsystem->RemoveFromRoot();
	});

	It("drains a pre-existing async flush through the isolated completion queue", [this]()
	{
		GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();
		Subsystem->InitializeTelemetry(
			TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
		Subsystem->Track(TEXT("async_before_blocking"));

		// Flush() returns while serialization is still owned by the background path. No
		// game-thread task graph or transport queue is drained before FlushBlocking starts,
		// so a registered request afterward proves the blocking pump consumed the isolated
		// pre-dispatch handoff rather than merely observing an already-issued request.
		const int32 RegisteredBefore =
			Subsystem->GetRegisteredTransportCompletionCountForTesting();
		Subsystem->Flush();
		const int32 AppliedBefore =
			Subsystem->GetAppliedTransportCompletionCountForTesting();
		// Libcurl may surface even an immediate loopback refusal after about two seconds.
		// Leave CI margin while still proving the isolated pump applies its completion.
		TestFalse(TEXT("dead-loopback async flush is not reported as delivered"),
			Subsystem->FlushBlocking(3000));
		TestTrue(TEXT("blocking pump dispatched the pre-existing async flush"),
			Subsystem->GetRegisteredTransportCompletionCountForTesting() > RegisteredBefore);
		TestTrue(TEXT("blocking pump applied the pre-existing async completion"),
			Subsystem->GetAppliedTransportCompletionCountForTesting() > AppliedBefore);
		TestTrue(TEXT("subsystem remains usable after the pre-existing flush timeout"),
			Subsystem->IsInitialized());

		// The status-0 retry is ticker-driven and intentionally cannot settle in this
		// blocking harness. Teardown must still persist exactly one copy of the tracked
		// batch, proving the late/retry path did not duplicate in-flight bookkeeping.
		Subsystem->Deinitialize();
		const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
		TestEqual(TEXT("async event persisted exactly once"),
			CountFlushBlockingEventsByName(Persisted, TEXT("async_before_blocking")), 1);
		TestEqual(TEXT("async session_start persisted exactly once"),
			CountFlushBlockingEventsByName(Persisted, TEXT("session_start")), 1);

		Subsystem->RemoveFromRoot();
	});

	It("loses no CJK attribute-heavy events split by the blocking byte cap", [this]()
	{
		GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();
		Subsystem->InitializeTelemetry(
			TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));

		TMap<FString, FString> Attributes;
		// This batch stays below 1 MiB if UTF-16 units are mistaken for bytes, but
		// exceeds it when common BMP CJK is costed at three UTF-8 bytes per unit.
		const FString HeavyValue = FString::ChrN(512, TEXT('\u6F22'));
		for (int32 Index = 0; Index < 30; ++Index)
		{
			Attributes.Add(
				FString::Printf(TEXT("heavy_key_%02d"), Index),
				HeavyValue);
		}
		const TMap<FString, double> Metrics;
		const int32 HeavyEventCount = 45;
		for (int32 Index = 0; Index < HeavyEventCount; ++Index)
		{
			Subsystem->TrackWithData(
				TEXT("blocking_heavy"), TEXT(""), FVector::ZeroVector,
				Attributes, Metrics);
		}

		const int32 RegisteredBefore =
			Subsystem->GetRegisteredTransportCompletionCountForTesting();
		TestFalse(TEXT("dead-loopback byte-split flush is not delivered"),
			Subsystem->FlushBlocking(1000));
		TestTrue(TEXT("CJK UTF-8 estimate split the wave into multiple requests"),
			Subsystem->GetRegisteredTransportCompletionCountForTesting()
				>= RegisteredBefore + 2);
		TestTrue(TEXT("subsystem survives the byte-split timeout"),
			Subsystem->IsInitialized());

		Subsystem->Deinitialize();
		const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
		TestEqual(TEXT("all byte-split events persist exactly once"),
			CountFlushBlockingEventsByName(Persisted, TEXT("blocking_heavy")),
			HeavyEventCount);
		TestEqual(TEXT("byte-split session_start persists exactly once"),
			CountFlushBlockingEventsByName(Persisted, TEXT("session_start")), 1);

		Subsystem->RemoveFromRoot();
	});

	It("times out fail-safe with the offline queue disabled and settles at teardown", [this]()
	{
		GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = false;

		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();

		// The dead-loopback retry remains tracked after the deadline; no speculative buffer
		// copy is created. This harness cannot produce a late 2xx or permanent 4xx, so it
		// verifies the other terminal path: immediate teardown clears the in-flight map via
		// PersistInFlightBatches, and FNullPersistence keeps the queue-off result volatile.
		Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
		Subsystem->Track(TEXT("blocking_event_no_queue"));

		const bool bDelivered = Subsystem->FlushBlocking(100);
		TestFalse(TEXT("undeliverable flush times out to false (queue off)"), bDelivered);
		TestTrue(TEXT("still initialized after a queue-off timeout"), Subsystem->IsInitialized());

		// Teardown must settle the still-tracked batch without persistence or a crash.
		Subsystem->Deinitialize();
		TestFalse(TEXT("no longer initialized after teardown"), Subsystem->IsInitialized());
		const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
		TestEqual(TEXT("nothing persisted when the offline queue is disabled"), Persisted.Num(), 0);

		Subsystem->RemoveFromRoot();
	});

	It("loses no events flushing a buffer larger than one batch", [this]()
	{
		GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

		UFramedashSubsystem* Subsystem = MakeSubsystem();
		Subsystem->AddToRoot();

		// Nothing can be delivered to the dead loopback port, so every tracked event must survive:
		// whatever is still buffered (deferred tail) plus whatever is tracked in flight (the
		// auto-flushed / capped wave) are all persisted at teardown. This guards no-loss across the
		// large-buffer / multi-wave path; the cap's boundedness (a timing property) is not asserted
		// here -- see the header note.
		Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
		const int32 BulkCount = 250;
		for (int32 Index = 0; Index < BulkCount; ++Index)
		{
			Subsystem->Track(TEXT("bulk_event"));
		}

		const bool bDelivered = Subsystem->FlushBlocking(200);
		TestFalse(TEXT("undeliverable large flush times out to false"), bDelivered);
		TestTrue(TEXT("still initialized after a large blocking flush"), Subsystem->IsInitialized());

		Subsystem->Deinitialize();
		const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
		TestEqual(TEXT("all bulk events survive the capped multi-wave flush"),
			CountFlushBlockingEventsByName(Persisted, TEXT("bulk_event")), BulkCount);
		TestEqual(TEXT("session_start survives"),
			CountFlushBlockingEventsByName(Persisted, TEXT("session_start")), 1);

		Subsystem->RemoveFromRoot();
	});
}

#endif
