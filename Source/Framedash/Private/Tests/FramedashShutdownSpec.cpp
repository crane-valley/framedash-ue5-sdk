// Copyright Crane Valley. All Rights Reserved.

#include "CoreMinimal.h"

// Dev-only AND opt-in. WITH_DEV_AUTOMATION_TESTS alone is not enough: BuildPlugin
// produces Development/Editor DLLs with that flag set, so gating on it would ship this
// spec inside the redistributable binaries. Its destructive BeforeEach clears the
// project's Saved/Framedash offline queue, so a game running product-filter automation
// would wipe its own persisted telemetry. FRAMEDASH_WITH_AUTOMATION_SPECS is defined to
// 1 only when Framedash.Build.cs sees FRAMEDASH_BUILD_AUTOMATION_SPECS=1 (set by the
// automation-spec CI job and the README local-run instructions), so the spec compiles
// ONLY for the dedicated automation build and never for BuildPlugin / a game project.
#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"

#include "FramedashSubsystem.h"
#include "FramedashSettings.h"
#include "FramedashPersistenceProvider.h"
#include "FramedashTypes.h"

// In-engine coverage for the UFramedashSubsystem shutdown/durability contract. The
// engine-independent GoogleTest harness (sdks/ue5/Tests) cannot reach this path:
// ShutdownTelemetry / PersistInFlightBatches / WaitForFailurePersistenceTasks and the
// offline-queue restore in InitializeInternal are all UE-type-coupled (FString, TArray,
// TFuture, and the file-backed FFilePersistence). Per the PLANS guardrail the event
// buffer and transport must NOT be extracted to pure C++, so their teardown contract is
// exercised here through the public subsystem API against a real on-disk offline queue.

namespace
{
	// Count queued events by name so a test can assert exactly which events reached the
	// persisted offline queue without depending on their order in the file.
	int32 CountEventsByName(const TArray<FFramedashEvent>& Events, const TCHAR* Name)
	{
		int32 Total = 0;
		for (const FFramedashEvent& Event : Events)
		{
			if (Event.EventName == Name)
			{
				++Total;
			}
		}
		return Total;
	}
}

BEGIN_DEFINE_SPEC(FFramedashShutdownDurabilitySpec, "Framedash.Shutdown.Durability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	// The subsystem reads bEnableOfflineQueue / bCaptureCameraRotation / bTrackDiskIo /
	// bAutoInitialize from the shared settings CDO, so each test mutates it and must
	// restore the editor's CDO afterward.
	bool bSavedAutoInitialize = false;
	bool bSavedEnableOfflineQueue = true;
	bool bSavedCaptureCameraRotation = true;
	bool bSavedTrackDiskIo = false;

	// UFramedashSubsystem is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so it
	// must be constructed with a UGameInstance outer -- a transient-package outer trips a
	// handled ensure. A bare, uninitialized GameInstance is a sufficient owner for driving
	// the public telemetry API directly: the shutdown/durability path never dereferences
	// the owning GameInstance. Rooting the returned subsystem keeps this owner alive via
	// the outer chain.
	UFramedashSubsystem* MakeSubsystem()
	{
		UGameInstance* Owner = NewObject<UGameInstance>(GEngine);
		return NewObject<UFramedashSubsystem>(Owner);
	}

END_DEFINE_SPEC(FFramedashShutdownDurabilitySpec)

void FFramedashShutdownDurabilitySpec::Define()
{
	BeforeEach([this]()
	{
		UFramedashSettings* Settings = GetMutableDefault<UFramedashSettings>();
		bSavedAutoInitialize = Settings->bAutoInitialize;
		bSavedEnableOfflineQueue = Settings->bEnableOfflineQueue;
		bSavedCaptureCameraRotation = Settings->bCaptureCameraRotation;
		bSavedTrackDiskIo = Settings->bTrackDiskIo;

		// A detached NewObject subsystem never runs the collection auto-init, but pin
		// these off so no other path can perturb the test. Camera capture is disabled
		// because a detached subsystem has no PlayerController to sample -- the read is
		// pointless noise for a durability test.
		Settings->bAutoInitialize = false;
		Settings->bCaptureCameraRotation = false;
		Settings->bTrackDiskIo = false;

		// Start every test from an empty queue file so counts are absolute.
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

	Describe("with the offline queue enabled", [this]()
	{
		It("persists events still buffered at teardown", [this]()
		{
			GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

			UFramedashSubsystem* Subsystem = MakeSubsystem();
			// Root against GC for the duration of the test: an async transport callback
			// may still be queued after teardown and would otherwise reference a
			// collected subsystem (it is dropped via the transport's alive flag, but
			// rooting removes any doubt while the synchronous body runs).
			Subsystem->AddToRoot();

			// Loopback http passes the endpoint-security check; nothing is flushed in
			// this case, so no request is ever made.
			Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
			// session_start is auto-tracked at init; add two more so the buffer holds
			// three unsent events at teardown (well under the 100-event flush trigger).
			Subsystem->Track(TEXT("buffered_event_a"));
			Subsystem->Track(TEXT("buffered_event_b"));
			Subsystem->Deinitialize();

			const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
			TestEqual(TEXT("buffered_event_a persisted"), CountEventsByName(Persisted, TEXT("buffered_event_a")), 1);
			TestEqual(TEXT("buffered_event_b persisted"), CountEventsByName(Persisted, TEXT("buffered_event_b")), 1);
			TestEqual(TEXT("session_start persisted"), CountEventsByName(Persisted, TEXT("session_start")), 1);

			Subsystem->RemoveFromRoot();
		});

		It("persists an in-flight batch that was flushed but never delivered", [this]()
		{
			GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

			UFramedashSubsystem* Subsystem = MakeSubsystem();
			Subsystem->AddToRoot();

			// Port 1 on loopback is unroutable, so the HTTP POST can never succeed. The
			// batch is recorded as in-flight synchronously inside Flush(), while the send
			// itself is dispatched to a background task plus a game-thread AsyncTask that
			// cannot run before this synchronous test body returns. At Deinitialize the
			// batch is therefore still open and must be drained by PersistInFlightBatches.
			Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
			Subsystem->Track(TEXT("inflight_event"));
			Subsystem->Flush();
			Subsystem->Deinitialize();

			const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
			TestEqual(TEXT("inflight_event persisted"), CountEventsByName(Persisted, TEXT("inflight_event")), 1);
			TestEqual(TEXT("session_start persisted"), CountEventsByName(Persisted, TEXT("session_start")), 1);

			Subsystem->RemoveFromRoot();
		});

		It("restores persisted events on a fresh Initialize", [this]()
		{
			GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = true;

			// Seed three durable events on disk before any subsystem exists.
			TArray<FFramedashEvent> Seed;
			for (int32 Index = 0; Index < 3; ++Index)
			{
				FFramedashEvent Event;
				Event.EventName = TEXT("restored_event");
				Event.SessionId = TEXT("seed-session");
				Event.TimestampUs = 1000 + Index;
				Seed.Add(MoveTemp(Event));
			}
			TestTrue(TEXT("seed written to disk"), FFilePersistence().Save(Seed));

			UFramedashSubsystem* Subsystem = MakeSubsystem();
			Subsystem->AddToRoot();
			Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
			TestTrue(TEXT("init with a seeded queue succeeded"), Subsystem->IsInitialized());

			// Restore has already happened (the three seed events are now in the buffer).
			// Wipe the on-disk file here so the teardown assertion measures ONLY what the
			// buffer drain re-persists, independent of whether restored events are also
			// left durable on disk until acknowledged -- this proves restore ran without
			// codifying the current restore-then-re-append duplication as the contract.
			FFilePersistence().Clear();

			// Tear down without flushing: the drain persists the three restored events plus
			// the auto-tracked session_start. Had restore not run, only session_start would
			// be drained and restored_event would be absent.
			Subsystem->Deinitialize();

			const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
			TestEqual(TEXT("restored events re-persisted"), CountEventsByName(Persisted, TEXT("restored_event")), 3);
			TestEqual(TEXT("session_start persisted"), CountEventsByName(Persisted, TEXT("session_start")), 1);

			Subsystem->RemoveFromRoot();
		});
	});

	Describe("with the offline queue disabled", [this]()
	{
		It("drops buffered events at teardown without crashing", [this]()
		{
			GetMutableDefault<UFramedashSettings>()->bEnableOfflineQueue = false;

			UFramedashSubsystem* Subsystem = MakeSubsystem();
			Subsystem->AddToRoot();
			Subsystem->InitializeTelemetry(TEXT("dummy-key"), TEXT("http://127.0.0.1:1"), TEXT("test-build"), TEXT(""));
			Subsystem->Track(TEXT("dropped_event"));
			// Fail-safe contract: teardown must not throw or crash even though the queue
			// is disabled -- the events are simply dropped.
			Subsystem->Deinitialize();

			TestFalse(TEXT("subsystem is no longer initialized"), Subsystem->IsInitialized());
			const TArray<FFramedashEvent> Persisted = FFilePersistence().Load();
			TestEqual(TEXT("nothing persisted when the offline queue is disabled"), Persisted.Num(), 0);

			Subsystem->RemoveFromRoot();
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
