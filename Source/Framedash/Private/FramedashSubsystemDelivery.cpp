// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashSubsystem.h"
#include "Framedash.h"
#include "FramedashSettings.h"
#include "FramedashEventBuffer.h"
#include "FramedashTransport.h"
#include "FramedashSessionManager.h"
#include "FramedashPerformanceCollector.h"
#include "FramedashSamplingPolicy.h"
#include "FramedashPersistenceProvider.h"
#include "FramedashFlushPolicy.h"
#include "FramedashFieldClamps.h"
#include "FramedashStringUtil.h"
#include "FramedashEngineCompat.h"
#include "FramedashIoStats.h"
#include "FramedashIoTrackingPlatformFile.h"
#include "FramedashMapLoadTimer.h"

#include "Async/Async.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Char.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "FramedashCameraMath.h"

void UFramedashSubsystem::Flush()
{
	if (bIsFlushing) return;
	if (!bInitialized || EventBuffer->IsEmpty()) return;

	bIsFlushing = true;
	EstimatedPayloadBytes = 0;
	TimeSinceLastFlush = 0.0f;

	TArray<FFramedashEvent> Events = EventBuffer->DequeueAll();

	int32 SendCount = Events.Num();

	// Durable-prefix ordering: never send fresh events ahead of an unacknowledged restored
	// prefix. When the buffer holds more than the prefix, send only the prefix this wave.
	const int32 PersistedEventsInBuffer = FMath::Min(PendingPersistedEventsToAck, Events.Num());
	if (PersistedEventsInBuffer > 0 && PersistedEventsInBuffer < SendCount)
	{
		SendCount = PersistedEventsInBuffer;
	}

	// A blocking wave stays at one normal batch, while the transport further halves an
	// attribute-heavy wave until each inline serialize/gzip estimate is at most 1 MiB and
	// checks the shared deadline between children. Deferred events stay buffered for the
	// next wave. The async cadence remains uncapped here because its CPU work stays off the
	// game thread.
	if (bObserveFlushDelivery && SendCount > FramedashConstants::MaxBatchSize)
	{
		SendCount = FramedashConstants::MaxBatchSize;
	}

	if (SendCount < Events.Num())
	{
		TArray<FFramedashEvent> DeferredEvents;
		DeferredEvents.Reserve(Events.Num() - SendCount);
		for (int32 Index = SendCount; Index < Events.Num(); ++Index)
		{
			DeferredEvents.Add(MoveTemp(Events[Index]));
		}
		Events.SetNum(SendCount);
		for (auto&& DeferredEvent : DeferredEvents)
		{
			EventBuffer->Enqueue(MoveTemp(DeferredEvent));
		}
		EstimatedPayloadBytes = FlushPolicy->EstimatePayloadBytes(DeferredEvents.Num());
	}

	int32 PersistedEventCount = 0;
	const uint64 BatchId = TrackInFlightBatch(Events, PersistedEventCount);

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	UE_LOG(LogFramedash, Log, TEXT("Flushing %d events."), Events.Num());
#else
	UE_LOG(LogFramedash, Verbose, TEXT("Flushing %d events."), Events.Num());
#endif
	Transport->SendBatch(
		MoveTemp(Events),
		[this, PersistedEventCount, BatchId](TArray<FFramedashEvent>&& FailedEvents, int32 FailedEventOffset)
		{
			// A synchronous FlushBlocking is waiting on this wave: record that a batch
			// failed and was queued for retry so it returns false (delivery unconfirmed)
			// even though DequeueAll already emptied the in-memory buffer. Read at
			// callback time on the game thread, so it also catches a pre-existing async
			// wave whose callback fires during the pump.
			if (bObserveFlushDelivery)
			{
				bBlockingFlushSawFailure = true;
			}
			const int32 PersistedFailureCount = FMath::Clamp(PersistedEventCount - FailedEventOffset, 0, FailedEvents.Num());
			if (PersistedFailureCount > 0)
			{
				FailedEvents.RemoveAt(0, PersistedFailureCount, FRAMEDASH_ALLOW_SHRINKING_NO);
			}
			// BatchId lets the queue-off path reject a stale failure after shutdown or close,
			// so only the request that still owns the tracked batch can re-enqueue it.
			PersistFailedEvents(MoveTemp(FailedEvents), BatchId);
		},
		[this, BatchId](int32 DeliveredLeadingEventCount)
		{
			CloseInFlightBatch(BatchId, DeliveredLeadingEventCount);
			bIsFlushing = false;
		},
		[this](int32  )
		{
			// The batch was permanently dropped WITHOUT delivery (endpoint insecure,
			// serialize failure, cross-origin redirect, or a poison/non-retryable HTTP
			// status such as a 401 invalid key). Both success and a poison-drop close with
			// the full count, so this distinct signal is the only way a synchronous
			// FlushBlocking can tell them apart: treat it as non-delivery so the call
			// returns false instead of falsely reporting success once the buffer drains.
			if (bObserveFlushDelivery)
			{
				bBlockingFlushSawFailure = true;
			}
		},
		// Serialize + dispatch synchronously while a blocking flush is observing, so its
		// outbound request does not depend on the game-thread task graph. Completion
		// isolation is uniform and does not branch on this dispatch choice.
		  bObserveFlushDelivery,
		BlockingDeadlineSeconds);
}

bool UFramedashSubsystem::FlushBlocking(int32 TimeoutMs)
{
	// GAME THREAD ONLY: the pump applies transport completions to game-thread-only state.
	// Off-thread, warn and refuse rather than risk a fatal assert (the never-crash rule;
	// mirrors SetPlayerId).
	if (!IsInGameThread())
	{
		UE_LOG(LogFramedash, Warning, TEXT("FlushBlocking must be called on the game thread. Ignoring."));
		return false;
	}
	if (!bInitialized || !Transport.IsValid() || !EventBuffer.IsValid())
	{
		return false;
	}
	// Endpoint-security fail-closed: sending is disabled, so a batch would be dropped
	// synchronously. Report failure now instead of pumping for a delivery that can
	// never happen (and instead of falsely returning true after the drop).
	if (!Transport->IsEndpointSecure())
	{
		return false;
	}
	// A non-positive budget cannot make progress; treat as an immediate timeout.
	if (TimeoutMs <= 0)
	{
		return false;
	}

	// Nothing buffered and no flush in flight: the documented contract returns true
	// ("or there was nothing to send") without entering the timed wait. Applying the
	// strict-budget verdict to an idle SDK could otherwise report false under a tiny
	// positive budget or a deschedule right after the deadline is computed.
	if (!bIsFlushing && EventBuffer->IsEmpty())
	{
		return true;
	}

	const int32 BudgetMs = FMath::Min(TimeoutMs, FramedashConstants::MaxFlushBlockingMs);
	const double Deadline = FPlatformTime::Seconds() + static_cast<double>(BudgetMs) / 1000.0;

	// Observe delivery for the duration of the wait so the shared flush callbacks
	// (which run on this thread during the pump) can distinguish delivery from a
	// retry-queued failure. Cleared on every exit so a later async cadence failure
	// cannot perturb a subsequent call.
	bBlockingFlushSawFailure = false;
	BlockingDeadlineSeconds = Deadline;
	bObserveFlushDelivery = true;

	// Drain any flush already in flight at call time. Background serialization and HTTP
	// completion both publish through transport-owned queues, so work that has not reached
	// ProcessRequest yet is observable without touching the game-thread task graph or the
	// process-global HTTP manager. Exit as soon as a failure is recorded: a split child
	// can fail while a sibling waits on a ticker retry that cannot run on this blocked
	// thread; waiting on it would burn the whole budget for an already-decided false
	// verdict. The outstanding request stays tracked and settles via the Tick drains.
	while (bIsFlushing && !bBlockingFlushSawFailure && FPlatformTime::Seconds() < Deadline)
	{
		PumpTelemetryDelivery();
	}

	// Then flush the buffer to empty, one bounded wave per iteration. The subsystem limits a
	// wave to MaxBatchSize, and the transport size-splits attribute-heavy batches so the
	// shared deadline is checked before and after each <=1 MiB estimated serialize/gzip
	// unit. A single event is the only accepted indivisible residual. On failure the normal
	// retention callbacks set bBlockingFlushSawFailure. Stop before a later wave: dispatch
	// advances the persisted-prefix cursor, but only contiguous delivery advances the disk
	// head, so crossing a failed wave could acknowledge the wrong durable events (#1317).
	while (!EventBuffer->IsEmpty() && !bBlockingFlushSawFailure &&
		FPlatformTime::Seconds() < Deadline)
	{
		Flush();
		// Same early exit as the pre-existing-flush drain above: once this wave has
		// recorded a failure, its ticker-scheduled retry cannot run on this blocked
		// thread, so waiting further only burns the remaining budget.
		while (bIsFlushing && !bBlockingFlushSawFailure && FPlatformTime::Seconds() < Deadline)
		{
			PumpTelemetryDelivery();
		}
	}

	bObserveFlushDelivery = false;
	BlockingDeadlineSeconds = TNumericLimits<double>::Max();

	// The loops above test the deadline only BEFORE an iteration, so a completion applied
	// during an iteration that crossed the deadline would otherwise still read as success.
	// Sample the clock once here, right after the drain that cleared bIsFlushing: the
	// late-confirmed batch stays acknowledged (never resent); the false return only
	// reports that confirmation landed past the caller's budget. Mirrors the Unity/Godot
	// strict-budget contract.
	const bool bWithinBudget = FPlatformTime::Seconds() < Deadline;
	const bool bDelivered = bWithinBudget &&
		!bIsFlushing && EventBuffer->IsEmpty() && !bBlockingFlushSawFailure;

	// Keep an unresolved BatchId tracked after timeout. Its actual result now owns the only
	// decision: success closes exactly once, transient exhaustion re-enqueues once when the
	// queue is off, and a permanent rejection closes without creating a retry copy. If the
	// subsystem shuts down first, PersistInFlightBatches runs before transport destruction
	// and applies the configured persistence policy.
	return bDelivered;
}

void UFramedashSubsystem::PumpTelemetryDelivery()
{
	// Background serialization and request completion each publish into a transport-owned
	// queue. Dispatch drains first so its request can register and even complete before this
	// iteration applies snapshots. Both queues exclude unrelated game work.
	// The old global FHttpManager tick could synchronously complete arbitrary game-owned
	// requests here, re-enter gameplay delegates, and run unrelated work past TimeoutMs.
	//
	// Scheduled ticker retries and reactive 413 redispatch are still not driven here; they
	// retain their existing deadline/no-loss behavior. Direct-socket fallback results use
	// the same isolated drain instead of relying on the game-thread task graph.
	if (Transport.IsValid())
	{
		Transport->DrainPendingDispatches();
		Transport->DrainCompletedRequests();
	}

	// Yield the core so the socket / OS makes progress instead of busy-spinning -- but
	// only while the awaited wave is still open. Sleeping after the terminal drain could
	// push the clock past the deadline and make the strict-budget verdict (sampled right
	// after the wait loops exit) misreport an on-time confirmation as an overrun.
	if (bIsFlushing)
	{
		FPlatformProcess::Sleep(FramedashConstants::BlockingPumpStepSeconds);
	}
}

uint64 UFramedashSubsystem::TrackInFlightBatch(const TArray<FFramedashEvent>& Events, int32& OutPersistedEventCount)
{
	OutPersistedEventCount = 0;
	// Track batches even when the offline queue is disabled so their actual outcome remains
	// authoritative after a FlushBlocking timeout: transient failure can re-enqueue once,
	// while success or permanent rejection removes the only retained copy. Shutdown also
	// clears this map before destroying the transport. With the queue off there is no
	// restored prefix, so the positional acknowledgement path remains inert.

	FScopeLock Lock(&InFlightCriticalSection);
	const uint64 BatchId = NextBatchId++;
	const int32 PersistedEventCount = FMath::Min(PendingPersistedEventsToAck, Events.Num());
	PendingPersistedEventsToAck -= PersistedEventCount;
	OutPersistedEventCount = PersistedEventCount;

	FInFlightBatch InFlightBatch;
	InFlightBatch.PersistedEventCount = PersistedEventCount;
	InFlightBatch.Events.Reserve(Events.Num() - PersistedEventCount);
	for (int32 Index = PersistedEventCount; Index < Events.Num(); ++Index)
	{
		InFlightBatch.Events.Add(Events[Index]);
	}
	InFlightBatches.Add(BatchId, MoveTemp(InFlightBatch));
	return BatchId;
}

void UFramedashSubsystem::CloseInFlightBatch(uint64 BatchId, int32 DeliveredLeadingEventCount)
{
	if (BatchId == 0)
	{
		return;
	}

	int32 PersistedEventCount = 0;
	{
		FScopeLock Lock(&InFlightCriticalSection);
		if (FInFlightBatch* Batch = InFlightBatches.Find(BatchId))
		{
			PersistedEventCount = Batch->PersistedEventCount;
			InFlightBatches.Remove(BatchId);
		}
	}

	const int32 DeliveredPersistedEventCount = FMath::Min(PersistedEventCount, DeliveredLeadingEventCount);
	if (DeliveredPersistedEventCount > 0 && PersistenceProvider.IsValid())
	{
		if (PersistenceProvider->DropOldest(DeliveredPersistedEventCount))
		{
			UE_LOG(LogFramedash, Log, TEXT("Acknowledged %d persisted event(s)."), DeliveredPersistedEventCount);
		}
		else
		{
			UE_LOG(LogFramedash, Warning, TEXT("Failed to acknowledge %d persisted event(s)."), DeliveredPersistedEventCount);
		}
	}
}

void UFramedashSubsystem::PersistInFlightBatches()
{
	if (!PersistenceProvider.IsValid())
	{
		return;
	}

	TArray<FFramedashEvent> Events;
	{
		FScopeLock Lock(&InFlightCriticalSection);
		for (auto&& Batch : InFlightBatches)
		{
			for (auto&& Event : Batch.Value.Events)
			{
				Events.Add(MoveTemp(Event));
			}
		}
		InFlightBatches.Empty();
	}

	if (Events.Num() == 0)
	{
		return;
	}

	if (!bOfflineQueueEnabled)
	{
		UE_LOG(LogFramedash, Warning, TEXT("Shutdown: offline queue disabled, dropping %d in-flight event(s)."), Events.Num());
	}
	else if (PersistenceProvider->Append(Events))
	{
		UE_LOG(LogFramedash, Log, TEXT("Shutdown: persisted %d in-flight event(s) for next run."), Events.Num());
	}
	else
	{
		UE_LOG(LogFramedash, Warning, TEXT("Shutdown: %d in-flight event(s) could not be persisted."), Events.Num());
	}
}

void UFramedashSubsystem::PersistFailedEvents(TArray<FFramedashEvent>&& Events, uint64 BatchId)
{
	if (Events.Num() == 0)
	{
		return;
	}

	if (!bOfflineQueueEnabled)
	{
		// No disk queue to persist to. Re-enqueue the failed events in the bounded
		// in-memory ring buffer instead of dropping them, so a transient send failure
		// does not lose telemetry -- honoring the no-loss contract (including a
		// FlushBlocking timeout) for the queue-off configuration too. This strictly
		// improves durability and adds no disk I/O; the events are retried on the next
		// flush. With the queue off there is no restored durable prefix, so a plain
		// Enqueue cannot violate the positional-ack invariant. Runs on the game thread
		// (the flush callback), same as every other EventBuffer access. If the buffer is
		// already gone (shutdown teardown) there is nothing to do but drop. BatchId presence
		// proves this callback still owns an outstanding batch; shutdown or a prior close
		// makes a stale result inert instead of repopulating the buffer.
		bool bStillTracked = false;
		{
			FScopeLock Lock(&InFlightCriticalSection);
			bStillTracked = InFlightBatches.Contains(BatchId);
		}
		if (bStillTracked && EventBuffer.IsValid())
		{
			for (auto&& Event : Events)
			{
				EventBuffer->Enqueue(MoveTemp(Event));
			}
		}
		return;
	}

	TSharedPtr<IPersistenceProvider, ESPMode::ThreadSafe> PersistenceProviderForFailure = PersistenceProvider;
	const int32 EventCount = Events.Num();
	if (!PersistenceProviderForFailure.IsValid())
	{
		UE_LOG(LogFramedash, Warning, TEXT("Failed to persist %d event(s) after transient send failure."), EventCount);
		return;
	}

	TFuture<void> PersistenceTask = Async(
		EAsyncExecution::ThreadPool,
		[PersistenceProviderForFailure, Events = MoveTemp(Events), EventCount]() mutable
		{
			const bool bSaved = PersistenceProviderForFailure->Append(Events);
			AsyncTask(ENamedThreads::GameThread, [EventCount, bSaved]()
			{
				if (bSaved)
				{
					UE_LOG(LogFramedash, Warning, TEXT("Persisted %d event(s) after transient send failure."), EventCount);
				}
				else
				{
					UE_LOG(LogFramedash, Warning, TEXT("Failed to persist %d event(s) after transient send failure."), EventCount);
				}
			});
		});

	{
		FScopeLock Lock(&FailurePersistenceCriticalSection);
		for (int32 Index = FailurePersistenceTasks.Num() - 1; Index >= 0; --Index)
		{
			if (FailurePersistenceTasks[Index].IsReady())
			{
				FailurePersistenceTasks.RemoveAtSwap(Index, 1, FRAMEDASH_ALLOW_SHRINKING_NO);
			}
		}
		FailurePersistenceTasks.Add(MoveTemp(PersistenceTask));
	}
}

void UFramedashSubsystem::WaitForFailurePersistenceTasks()
{
	while (true)
	{
		TArray<TFuture<void>> Tasks;
		{
			FScopeLock Lock(&FailurePersistenceCriticalSection);
			if (FailurePersistenceTasks.Num() == 0)
			{
				return;
			}
			Tasks = MoveTemp(FailurePersistenceTasks);
		}

		for (auto&& Task : Tasks)
		{
			Task.Wait();
		}
	}
}
