// Copyright Crane Valley. All Rights Reserved.

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

#include "Async/Async.h"
#include "Misc/EngineVersion.h"

UFramedashSubsystem::UFramedashSubsystem()
{
}

void UFramedashSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UFramedashSettings* Settings = GetDefault<UFramedashSettings>();

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	if (!Settings)
	{
		UE_LOG(LogFramedash, Warning, TEXT("UFramedashSettings CDO is null — auto-init skipped."));
		return;
	}
	UE_LOG(LogFramedash, Log, TEXT("Settings: bAutoInitialize=%s, ApiKey=%s, Endpoint=%s"),
		Settings->bAutoInitialize ? TEXT("true") : TEXT("false"),
		Settings->ApiKey.IsEmpty() ? TEXT("(empty)") : TEXT("(set)"),
		*Settings->EndpointUrl);
#endif

	if (Settings && Settings->bAutoInitialize && !Settings->ApiKey.IsEmpty())
	{
		InitializeInternal(Settings->ApiKey, Settings->EndpointUrl, Settings->BuildId, Settings->PlayerId, Settings->SamplingRate);
	}
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	else if (Settings)
	{
		UE_LOG(LogFramedash, Log, TEXT("Auto-init skipped: bAutoInitialize=%s, ApiKey empty=%s"),
			Settings->bAutoInitialize ? TEXT("true") : TEXT("false"),
			Settings->ApiKey.IsEmpty() ? TEXT("true") : TEXT("false"));
	}
#endif
}

void UFramedashSubsystem::Deinitialize()
{
	ShutdownTelemetry();
	DestroyComponents();
	Super::Deinitialize();
}

void UFramedashSubsystem::InitializeTelemetry(const FString& ApiKey, const FString& EndpointUrl, const FString& BuildId, const FString& PlayerId)
{
	if (bInitialized)
	{
		UE_LOG(LogFramedash, Warning, TEXT("SDK already initialized. Ignoring duplicate call."));
		return;
	}

	const UFramedashSettings* Settings = GetDefault<UFramedashSettings>();
	const float SamplingRate = Settings ? Settings->SamplingRate : 1.0f;

	const FString ResolvedApiKey = ApiKey.IsEmpty()
		? (Settings ? Settings->ApiKey : TEXT(""))
		: ApiKey;
	const FString ResolvedEndpoint = EndpointUrl.IsEmpty()
		? (Settings ? Settings->EndpointUrl : TEXT("https://ingest.framedash.dev/v1/events"))
		: EndpointUrl;
	const FString ResolvedBuildId = BuildId.IsEmpty()
		? (Settings ? Settings->BuildId : TEXT(""))
		: BuildId;
	const FString TrimmedPlayerId = PlayerId.TrimStartAndEnd();
	const FString ResolvedPlayerId = TrimmedPlayerId.IsEmpty()
		? (Settings ? Settings->PlayerId : TEXT(""))
		: TrimmedPlayerId;

	InitializeInternal(ResolvedApiKey, ResolvedEndpoint, ResolvedBuildId, ResolvedPlayerId, SamplingRate);
}

void UFramedashSubsystem::SetPlayerId(const FString& PlayerId)
{
	if (!bInitialized)
	{
		UE_LOG(LogFramedash, Warning, TEXT("SDK not initialized. Call InitializeTelemetry() first."));
		return;
	}
	SessionManager->SetPlayerId(PlayerId);
}

void UFramedashSubsystem::InitializeInternal(const FString& ApiKey, const FString& EndpointUrl, const FString& BuildId, const FString& PlayerId, float SamplingRate)
{
	if (ApiKey.IsEmpty())
	{
		UE_LOG(LogFramedash, Error, TEXT("API key is required. Call InitializeTelemetry() with a valid key."));
		return;
	}

	CachedApiKey = ApiKey;
	CachedEndpointUrl = EndpointUrl;
	CachedBuildId = BuildId;
	CachedPlatform = FPlatformProperties::IniPlatformName();
	CachedEngineVersion = FEngineVersion::Current().ToString();

	EventBuffer = MakePimpl<FFramedashEventBuffer>(FramedashConstants::EventBufferCapacity);
	Transport = MakePimpl<FFramedashTransport>(CachedEndpointUrl, CachedApiKey);
	SessionManager = MakePimpl<FFramedashSessionManager>(PlayerId);
	PerformanceCollector = MakePimpl<FFramedashPerformanceCollector>();
	SamplingPolicy = MakePimpl<FFramedashSamplingPolicy>(SamplingRate);
	FlushPolicy = MakePimpl<Framedash::FFlushPolicy>(
		FramedashConstants::MaxBatchSize,
		FramedashConstants::MaxPayloadBytes,
		FramedashConstants::FlushIntervalSeconds,
		FramedashConstants::EstimatedBytesPerEvent);
	const UFramedashSettings* Settings = GetDefault<UFramedashSettings>();
	bOfflineQueueEnabled = !(Settings && !Settings->bEnableOfflineQueue);
	if (!bOfflineQueueEnabled)
	{
		PersistenceProvider = MakeShared<FNullPersistence, ESPMode::ThreadSafe>();
	}
	else
	{
		PersistenceProvider = MakeShared<FFilePersistence, ESPMode::ThreadSafe>();
	}
	PendingPersistedEventsToAck = 0;

	TArray<FFramedashEvent> RestoredEvents = PersistenceProvider->Load();
	if (RestoredEvents.Num() > 0)
	{
		for (FFramedashEvent& RestoredEvent : RestoredEvents)
		{
			EventBuffer->Enqueue(MoveTemp(RestoredEvent));
		}
		EstimatedPayloadBytes += FlushPolicy->EstimatePayloadBytes(RestoredEvents.Num());
		PendingPersistedEventsToAck = RestoredEvents.Num();
		UE_LOG(LogFramedash, Log, TEXT("Restored %d persisted event(s) to the offline queue."), RestoredEvents.Num());
	}

	// Tick is driven by FTickableGameObject — no manual ticker registration needed.
	// IsTickable() returns true once bInitialized is set.

	bInitialized = true;
	UE_LOG(LogFramedash, Log, TEXT("Framedash SDK v%s initialized. Session: %s"),
		FRAMEDASH_SDK_VERSION, *SessionManager->GetSessionId());

	// Auto-track session_start so the server always sees at least one event.
	// Source=Automated bypasses sampling to guarantee delivery.
	Track(TEXT("session_start"), TEXT(""), FVector::ZeroVector, EFramedashTelemetrySource::Automated);

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	UE_LOG(LogFramedash, Log, TEXT("  Endpoint: %s"), *CachedEndpointUrl);
	UE_LOG(LogFramedash, Log, TEXT("  Platform: %s | Engine: %s | SamplingRate: %.2f"),
		*CachedPlatform, *CachedEngineVersion, SamplingRate);
#endif
}

void UFramedashSubsystem::ShutdownTelemetry()
{
	if (!bInitialized) return;

	const int32 DroppedCount = EventBuffer.IsValid() ? EventBuffer->Count() : 0;
	if (DroppedCount > 0)
	{
		TArray<FFramedashEvent> Events = EventBuffer->DequeueAll();
		if (!bOfflineQueueEnabled)
		{
			UE_LOG(LogFramedash, Warning, TEXT("Shutdown: offline queue disabled, dropping %d buffered event(s)."), DroppedCount);
		}
		else if (PersistenceProvider.IsValid() && PersistenceProvider->Append(Events))
		{
			UE_LOG(LogFramedash, Log, TEXT("Shutdown: persisted %d buffered event(s) for next run."), DroppedCount);
		}
		else
		{
			UE_LOG(LogFramedash, Warning, TEXT("Shutdown: %d buffered event(s) could not be persisted."), DroppedCount);
		}
	}
	PersistInFlightBatches();
	WaitForFailurePersistenceTasks();

	bInitialized = false;
	UE_LOG(LogFramedash, Log, TEXT("Framedash SDK shut down."));
}

void UFramedashSubsystem::DestroyComponents()
{
	FlushPolicy.Reset();
	SamplingPolicy.Reset();
	PerformanceCollector.Reset();
	SessionManager.Reset();
	Transport.Reset();
	EventBuffer.Reset();
	PersistenceProvider.Reset();
}

void UFramedashSubsystem::Track(
	const FString& EventName,
	const FString& MapId,
	FVector Position,
	EFramedashTelemetrySource Source)
{
	TrackWithData(EventName, MapId, Position, TMap<FString, FString>(), TMap<FString, double>(), Source);
}

void UFramedashSubsystem::TrackWithData(
	const FString& EventName,
	const FString& MapId,
	FVector Position,
	const TMap<FString, FString>& Attributes,
	const TMap<FString, double>& Metrics,
	EFramedashTelemetrySource Source)
{
	if (!bInitialized)
	{
		UE_LOG(LogFramedash, Warning, TEXT("SDK not initialized. Call InitializeTelemetry() first."));
		return;
	}

	if (EventName.IsEmpty())
	{
		UE_LOG(LogFramedash, Warning, TEXT("EventName must not be empty. Event dropped."));
		return;
	}

	// One-time warning when developer-initiated events are sent without player_id.
	// Skip for Automated events (session_start, perf_heartbeat) — those fire before
	// the developer has a chance to call SetPlayerId().
	if (!bWarnedEmptyPlayerId && Source == EFramedashTelemetrySource::Player && SessionManager->GetPlayerId().IsEmpty())
	{
		bWarnedEmptyPlayerId = true;
		UE_LOG(LogFramedash, Warning, TEXT("No player_id set. Events will be sent as anonymous. Call SetPlayerId() to associate events with a player."));
	}

	// Normalize event name first so sampling and the wire-side event use the
	// same key — overrides registered for long names must match the truncated
	// form that actually leaves the SDK.
	FString SafeEventName = TruncateString(EventName, FramedashConstants::MaxEventNameLength);

	// Sampling check — skip expensive work if event is dropped.
	// Automated events (heartbeat, session_start) bypass sampling to ensure
	// backend always has session continuity and performance baselines.
	if (Source != EFramedashTelemetrySource::Automated && !SamplingPolicy->ShouldSample(SafeEventName))
	{
		return;
	}

	FFramedashPerformanceSnapshot Perf = PerformanceCollector->Collect();

	// Build event
	FFramedashEvent Evt;
	Evt.EventName = MoveTemp(SafeEventName);

	// Microsecond-precision UTC timestamp
	const FDateTime UtcNow = FDateTime::UtcNow();
	// FDateTime ticks are 100ns intervals since Jan 1, 0001
	// Convert to Unix epoch microseconds
	const int64 UnixEpochTicks = FDateTime(1970, 1, 1).GetTicks();
	Evt.TimestampUs = (UtcNow.GetTicks() - UnixEpochTicks) / 10; // 100ns -> us

	Evt.SessionId = SessionManager->GetSessionId();
	Evt.PlayerId = SessionManager->GetPlayerId();
	Evt.Position = Position;
	Evt.MapId = MapId;
	Evt.Fps = Perf.Fps;
	Evt.FrameTimeMs = Perf.FrameTimeMs;
	Evt.MemoryUsedBytes = Perf.MemoryUsedBytes;
	Evt.GpuTimeMs = Perf.GpuTimeMs;
	Evt.GameThreadMs = Perf.GameThreadMs;
	Evt.RenderThreadMs = Perf.RenderThreadMs;
	Evt.Source = Source;
	Evt.BuildId = CachedBuildId;
	Evt.Platform = CachedPlatform;
	Evt.EngineVersion = CachedEngineVersion;

	// Copy attributes (with limit)
	int32 AttrCount = 0;
	for (const auto& Pair : Attributes)
	{
		if (AttrCount >= FramedashConstants::MaxAttributePairs) break;
		Evt.Attributes.Add(Pair.Key, Pair.Value);
		++AttrCount;
	}

	// Copy metrics (with limit)
	int32 MetricCount = 0;
	for (const auto& Pair : Metrics)
	{
		if (MetricCount >= FramedashConstants::MaxMetricPairs) break;
		Evt.Metrics.Add(Pair.Key, Pair.Value);
		++MetricCount;
	}

	EventBuffer->Enqueue(MoveTemp(Evt));

	EstimatedPayloadBytes += FlushPolicy->GetBytesPerEventEstimate();

	if (FlushPolicy->ShouldRequestFlush(EventBuffer->Count(), EstimatedPayloadBytes))
	{
		Flush();
	}
}

void UFramedashSubsystem::SetEventSamplingRate(const FString& EventName, float Rate)
{
	if (!bInitialized || !SamplingPolicy.IsValid()) return;
	const FString NormalizedEventName = TruncateString(EventName, FramedashConstants::MaxEventNameLength);
	SamplingPolicy->SetEventRate(NormalizedEventName, Rate);
}

bool UFramedashSubsystem::RemoveEventSamplingRate(const FString& EventName)
{
	if (!bInitialized || !SamplingPolicy.IsValid()) return false;
	const FString NormalizedEventName = TruncateString(EventName, FramedashConstants::MaxEventNameLength);
	return SamplingPolicy->RemoveEventRate(NormalizedEventName);
}

void UFramedashSubsystem::Flush()
{
	if (bIsFlushing) return;
	if (!bInitialized || EventBuffer->IsEmpty()) return;

	bIsFlushing = true;
	EstimatedPayloadBytes = 0;
	TimeSinceLastFlush = 0.0f;

	TArray<FFramedashEvent> Events = EventBuffer->DequeueAll();
	const int32 PersistedEventsInBuffer = FMath::Min(PendingPersistedEventsToAck, Events.Num());
	if (PersistedEventsInBuffer > 0 && PersistedEventsInBuffer < Events.Num())
	{
		TArray<FFramedashEvent> DeferredEvents;
		DeferredEvents.Reserve(Events.Num() - PersistedEventsInBuffer);
		for (int32 Index = PersistedEventsInBuffer; Index < Events.Num(); ++Index)
		{
			DeferredEvents.Add(MoveTemp(Events[Index]));
		}
		Events.SetNum(PersistedEventsInBuffer);
		for (FFramedashEvent& DeferredEvent : DeferredEvents)
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
		[this, PersistedEventCount](TArray<FFramedashEvent>&& FailedEvents, int32 FailedEventOffset)
		{
			// Restored events are already durable at the head of the queue file.
			const int32 PersistedFailureCount = FMath::Clamp(PersistedEventCount - FailedEventOffset, 0, FailedEvents.Num());
			if (PersistedFailureCount > 0)
			{
				FailedEvents.RemoveAt(0, PersistedFailureCount, EAllowShrinking::No);
			}
			PersistFailedEvents(MoveTemp(FailedEvents));
		},
		[this, BatchId](int32 DeliveredLeadingEventCount)
		{
			CloseInFlightBatch(BatchId, DeliveredLeadingEventCount);
			bIsFlushing = false;
		});
}

uint64 UFramedashSubsystem::TrackInFlightBatch(const TArray<FFramedashEvent>& Events, int32& OutPersistedEventCount)
{
	OutPersistedEventCount = 0;
	if (!bOfflineQueueEnabled)
	{
		return 0;
	}

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
		for (TPair<uint64, FInFlightBatch>& Batch : InFlightBatches)
		{
			for (FFramedashEvent& Event : Batch.Value.Events)
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

void UFramedashSubsystem::PersistFailedEvents(TArray<FFramedashEvent>&& Events)
{
	if (Events.Num() == 0 || !bOfflineQueueEnabled)
	{
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
				FailurePersistenceTasks.RemoveAtSwap(Index, 1, EAllowShrinking::No);
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

		for (TFuture<void>& Task : Tasks)
		{
			Task.Wait();
		}
	}
}

void UFramedashSubsystem::Tick(float DeltaTime)
{
	TimeSinceLastFlush += DeltaTime;
	TimeSinceLastHeartbeat += DeltaTime;

	// Periodic performance heartbeat — auto-collects metrics without game code
	if (TimeSinceLastHeartbeat >= FramedashConstants::HeartbeatIntervalSeconds)
	{
		TimeSinceLastHeartbeat = 0.0f;
		Track(TEXT("perf_heartbeat"), TEXT(""), FVector::ZeroVector, EFramedashTelemetrySource::Automated);
	}

	if (FlushPolicy.IsValid() && FlushPolicy->ShouldFlush(/*bFlushRequested=*/false, TimeSinceLastFlush))
	{
		// Reset timer unconditionally — Flush() may early-return when buffer is
		// empty, but we must not call it every frame after the interval elapses.
		TimeSinceLastFlush = 0.0f;
		Flush();
	}
}

FString UFramedashSubsystem::GetSessionId() const
{
	if (!bInitialized || !SessionManager.IsValid())
	{
		return FString();
	}
	return SessionManager->GetSessionId();
}

FString UFramedashSubsystem::TruncateString(const FString& Input, int32 MaxLength)
{
	if (Input.Len() <= MaxLength) return Input;
	return Input.Left(MaxLength);
}
