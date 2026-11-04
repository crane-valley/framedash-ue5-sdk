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
	// Balance this subsystem's Install() with exactly one Disable(). Enablement is
	// reference-counted, so metering only actually stops once the LAST subsystem
	// that wanted it shuts down -- a still-running GameInstance keeps it on. The
	// wrapper stays chained either way (unchaining mid-run is unsafe); Disable() at
	// zero only flips the atomic enabled flag. Skip entirely if this subsystem never
	// installed, so it can never decrement another instance's count.
	if (bTrackDiskIoInstalled)
	{
		FFramedashIoTrackingPlatformFile::Disable();
		bTrackDiskIoInstalled = false;
	}
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

void UFramedashSubsystem::BeginAutomatedSession(const FString& BuildId, const FString& Branch, const FString& Commit, const FString& Scenario)
{
	if (!bInitialized)
	{
		UE_LOG(LogFramedash, Warning, TEXT("SDK not initialized. Call InitializeTelemetry() before BeginAutomatedSession()."));
		return;
	}
	// No metadata at all (e.g. BeginAutomatedSessionFromEnvironment with the FRAMEDASH_*
	// vars unset) is a true no-op: do not start an override or touch session attributes,
	// so a later End cannot clear state this call never set.
	if (BuildId.IsEmpty() && Branch.IsEmpty() && Commit.IsEmpty() && Scenario.IsEmpty())
	{
		return;
	}
	// Stamp events with the CI build_id in preference to CachedBuildId (see SessionBuildId
	// and TrackWithData); CachedBuildId itself is never overwritten, so a re-init or a later
	// config change can never strand a candidate id. Each Begin fully (re)defines the
	// session: set the override (truncated to the ingest cap like the init path) when a
	// build id is supplied, else clear it back to the CachedBuildId fallback -- the same
	// replace-don't-merge semantics as the ci.* attributes below, so no stale build_id
	// leaks from a prior session.
	SessionBuildId = BuildId.IsEmpty() ? FString() : TruncateString(BuildId, FramedashConstants::MaxBuildIdLength);
	// Replace any prior session attributes, value-clamped to the ingest cap here (once per
	// session) so the per-event stamp in TrackWithData can copy them without re-truncating --
	// this keeps the periodic perf_heartbeat off the per-attribute allocation path during a
	// CI profiling session. The ci.* keys are fixed and well under the key cap, so only the
	// values need clamping.
	SessionAttributes.Reset();
	if (!Branch.IsEmpty()) SessionAttributes.Add(TEXT("ci.branch"), TruncateString(Branch, FramedashConstants::MaxAttributeValueLength));
	if (!Commit.IsEmpty()) SessionAttributes.Add(TEXT("ci.commit"), TruncateString(Commit, FramedashConstants::MaxAttributeValueLength));
	if (!Scenario.IsEmpty()) SessionAttributes.Add(TEXT("ci.scenario"), TruncateString(Scenario, FramedashConstants::MaxAttributeValueLength));
}

void UFramedashSubsystem::BeginAutomatedSessionFromEnvironment()
{
	BeginAutomatedSession(
		FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEDASH_BUILD_ID")),
		FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEDASH_GIT_BRANCH")),
		FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEDASH_GIT_COMMIT")),
		FPlatformMisc::GetEnvironmentVariable(TEXT("FRAMEDASH_TEST_SCENARIO")));
}

void UFramedashSubsystem::EndAutomatedSession()
{
	SessionAttributes.Reset();
	// Drop the automated-session build_id override so post-session events carry
	// CachedBuildId again and are not grouped under the candidate build.
	SessionBuildId.Empty();
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
	// Truncate the cached string fields once here so every event stamps a
	// wire-valid value (ingest drops the whole batch if any exceeds its cap).
	// FEngineVersion::ToString() in particular can carry a long custom branch name.
	CachedBuildId = TruncateString(BuildId, FramedashConstants::MaxBuildIdLength);
	CachedPlatform = TruncateString(FPlatformProperties::IniPlatformName(), FramedashConstants::MaxPlatformLength);
	CachedEngineVersion = TruncateString(FEngineVersion::Current().ToString(), FramedashConstants::MaxEngineVersionLength);

	// Reset per-session scalar state so a Shutdown()+re-init starts clean. Seed
	// LastTickSeconds to now: a stale value would make the first post-reinit Tick
	// accumulate the entire shutdown gap as one RealDelta and fire a premature
	// flush/heartbeat. Seeding to now (rather than 0) also lets Tick drop its
	// first-call branch.
	LastTickSeconds = FPlatformTime::Seconds();
	TimeSinceLastFlush = 0.0f;
	TimeSinceLastHeartbeat = 0.0f;
	EstimatedPayloadBytes = 0;
	bWarnedEmptyPlayerId = false;
	RejectedDurablePrefixEventCount = 0;
	// Reset the single-flight flush guard too. The transport is recreated below, so
	// a prior in-flight flush's OnClosed is dropped by its now-stale AliveFlag and
	// would never clear this flag; left stuck true it would silently halt every
	// flush for the new session. Because that stale callback is dropped, clearing
	// the flag here cannot race a prior session's in-flight send.
	bIsFlushing = false;
	// A fresh session owns no automated-session build_id override or ci.* tags (a prior
	// Begin without an End must not keep stamping events under the candidate build).
	// SessionBuildId / SessionAttributes are only set via BeginAutomatedSession.
	SessionBuildId.Empty();
	SessionAttributes.Reset();

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
	bCaptureCameraRotation = Settings ? Settings->bCaptureCameraRotation : true;

	// Opt-in disk-IO metering: chain the IPlatformFile read wrapper so io.* window
	// metrics attach to perf_heartbeat. Installed once for the process lifetime and
	// re-enabled on a re-init; failure is swallowed inside Install() (fail-safe).
	// Enablement is reference-counted across GameInstances, so remember whether THIS
	// subsystem installed and balance it with exactly one Disable() in Deinitialize.
	// The manual ReportIoSample() feed works regardless of this setting. Reset the
	// per-session io.* attach gates first so a fresh session starts clean.
	bTrackDiskIoInstalled = false;
	bIoManualFeedAccepted = false;

	// Fresh map/level load-time timer per session so a Begin from a prior session can
	// never complete into this one. The loaded map name is cleared alongside it.
	MapLoadTimer = MakePimpl<Framedash::FMapLoadTimer>();
	PendingMapLoadName.Empty();

	if (Settings && Settings->bTrackDiskIo)
	{
		FFramedashIoTrackingPlatformFile::Install();
		bTrackDiskIoInstalled = true;
	}

	// Latch the memory-detail setting for this session. A later session with the
	// setting off emits no mem.* keys even if an earlier session in the same process
	// collected them. Start the session blind (no cached sample) so a fresh session
	// never attaches stale mem.* from a prior one.
	bTrackMemoryDetail = Settings && Settings->bTrackMemoryDetail;
	bMemorySampleValid = false;
	CachedMemMask = 0;
	// Start the persistent heartbeat map clean for a fresh session (this subsystem may
	// be re-initialized in the same process) and clear the mem key-set diff state.
	HeartbeatMetrics.Reset();
	PrevMemHeartbeatMask = 0;

	// Seed this subsystem's IO baseline to the current process-global cumulative
	// totals, so its first heartbeat window measures only reads from now on -- not
	// the entire process history (which could be large, e.g. a late re-init after
	// heavy loading) and not a stale baseline from a prior session.
	{
		const Framedash::FIoSnapshot IoNow = Framedash::GlobalIoStats().ReadCumulative();
		IoBaselineBytes = IoNow.ReadBytes;
		IoBaselineTimeMs = IoNow.ReadTimeMs;
		IoBaselineOps = IoNow.ReadOps;
	}
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
		for (auto&& RestoredEvent : RestoredEvents)
		{
			EventBuffer->Enqueue(MoveTemp(RestoredEvent));
		}
		EstimatedPayloadBytes += FlushPolicy->EstimatePayloadBytes(RestoredEvents.Num());
		PendingPersistedEventsToAck = RestoredEvents.Num();
		UE_LOG(LogFramedash, Log, TEXT("Restored %d persisted event(s) to the offline queue."), RestoredEvents.Num());
	}


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
		else
		{
			// Restored events remain at the head of the durable queue until delivery is
			// acknowledged. Re-appending that prefix would duplicate it on every offline restart.
			const int32 AlreadyPersistedCount =
				FMath::Min(PendingPersistedEventsToAck, Events.Num());
			if (AlreadyPersistedCount > 0)
			{
				Events.RemoveAt(0, AlreadyPersistedCount, FRAMEDASH_ALLOW_SHRINKING_NO);
				UE_LOG(LogFramedash, Verbose,
					TEXT("Shutdown: %d restored event(s) already durable, not re-appending."),
					AlreadyPersistedCount);
			}
			const int32 FreshEventCount = Events.Num();
			if (FreshEventCount > 0 && PersistenceProvider.IsValid() && PersistenceProvider->Append(Events))
			{
				UE_LOG(LogFramedash, Log, TEXT("Shutdown: persisted %d buffered event(s) for next run."), FreshEventCount);
			}
			else if (FreshEventCount > 0)
			{
				UE_LOG(LogFramedash, Warning, TEXT("Shutdown: %d buffered event(s) could not be persisted."), FreshEventCount);
			}
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
	MapLoadTimer.Reset();
}

void UFramedashSubsystem::UpdateCachedCameraRotation()
{
	// Called from Tick (game thread), where reading the World / PlayerController /
	// PlayerCameraManager UObjects is safe. The result is cached so each tracked
	// event is stamped with the latest sample without re-reading these UObjects.
	float NewYaw = 0.0f;
	float NewPitch = 0.0f;
	bool bHas = false;

	if (!IsRunningDedicatedServer())
	{
		const UWorld* World = GetWorld();
		const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (IsValid(PC) && IsValid(PC->PlayerCameraManager))
		{
			const FRotator ViewRotation = PC->PlayerCameraManager->GetCameraRotation();
			if (FMath::IsFinite(ViewRotation.Yaw) && FMath::IsFinite(ViewRotation.Pitch))
			{
				NewYaw = Framedash::NormalizeYawDegrees(ViewRotation.Yaw);
				NewPitch = Framedash::NormalizePitchDegrees(ViewRotation.Pitch);
				bHas = true;
			}
		}
	}

	FScopeLock Lock(&CameraRotationCS);
	bHasCachedCamera = bHas;
	CachedCameraYaw = NewYaw;
	CachedCameraPitch = NewPitch;
}

void UFramedashSubsystem::CaptureCameraRotation(FFramedashEvent& Evt) const
{
	// Reads the snapshot cached on the game thread by UpdateCachedCameraRotation, so
	// it touches no game-thread-only UObjects. Both fields are set together or unset.
	FScopeLock Lock(&CameraRotationCS);
	if (bHasCachedCamera)
	{
		Evt.CameraYaw = CachedCameraYaw;
		Evt.CameraPitch = CachedCameraPitch;
	}
}

void UFramedashSubsystem::RefreshMemorySample()
{
	if (!PerformanceCollector.IsValid())
	{
		return;
	}
	// Thin engine reads -> engine-independent selection -> SCALAR cache. Storing into
	// scalars (mask + doubles) instead of a TMap keeps this allocation-free, which is
	// required because this runs on the zero-alloc heartbeat path. An absent category
	// clears its bit, so it stays absent everywhere the cache is read.
	const Framedash::FMemoryMetrics Mem =
		Framedash::SelectMemoryMetrics(PerformanceCollector->SampleMemoryDetail());
	CachedMemMask = Framedash::MemoryPresenceMask(Mem);
	CachedMemVram = static_cast<double>(Mem.Vram);
	CachedMemTextures = static_cast<double>(Mem.Textures);
	CachedMemMeshes = static_cast<double>(Mem.Meshes);
	CachedMemAudio = static_cast<double>(Mem.Audio);
	bMemorySampleValid = true;
}

void UFramedashSubsystem::EmitPerformanceHeartbeat()
{
	// The persistent HeartbeatMetrics map is maintained IN PLACE (never Reset here)
	// so the steady-state heartbeat allocates no heap (CLAUDE.md rule). io.* keys are
	// overwritten via FindOrAdd; mem.* keys are diffed so only a key-set transition
	// touches the map. All keys are well under MaxMetricPairs and TrackWithData
	// drops non-finite values anyway.

	// io.* window metrics are attached only when this session activated a source.
	// Process-global counters may contain reads from earlier Editor/PIE sessions.
	if (bTrackDiskIoInstalled || bIoManualFeedAccepted)
	{
		const Framedash::FIoSnapshot IoNow = Framedash::GlobalIoStats().ReadCumulative();
		Framedash::FIoSnapshot IoBaseline;
		IoBaseline.ReadBytes = IoBaselineBytes;
		IoBaseline.ReadTimeMs = IoBaselineTimeMs;
		IoBaseline.ReadOps = IoBaselineOps;
		const Framedash::FIoSnapshot IoWindow = IoNow.Since(IoBaseline);
		IoBaselineBytes = IoNow.ReadBytes;
		IoBaselineTimeMs = IoNow.ReadTimeMs;
		IoBaselineOps = IoNow.ReadOps;
		static const FString IoKeyReadBytes(TEXT("io.read_bytes"));
		static const FString IoKeyReadTimeMs(TEXT("io.read_time_ms"));
		static const FString IoKeyReadOps(TEXT("io.read_ops"));
		HeartbeatMetrics.FindOrAdd(IoKeyReadBytes) = static_cast<double>(IoWindow.ReadBytes);
		HeartbeatMetrics.FindOrAdd(IoKeyReadTimeMs) = IoWindow.ReadTimeMs;
		HeartbeatMetrics.FindOrAdd(IoKeyReadOps) = static_cast<double>(IoWindow.ReadOps);
	}

	// mem.* keys are diffed so their steady-state overwrite path stays allocation-free.
	if (bTrackMemoryDetail)
	{
		RefreshMemorySample();
		static const FString MemKeyVram(TEXT(FRAMEDASH_MEM_KEY_VRAM));
		static const FString MemKeyTextures(TEXT(FRAMEDASH_MEM_KEY_TEXTURES));
		static const FString MemKeyMeshes(TEXT(FRAMEDASH_MEM_KEY_MESHES));
		static const FString MemKeyAudio(TEXT(FRAMEDASH_MEM_KEY_AUDIO));
		const uint32 StaleMask = Framedash::StaleMemoryKeysMask(PrevMemHeartbeatMask, CachedMemMask);
		if (StaleMask & Framedash::kMemBitVram) { HeartbeatMetrics.Remove(MemKeyVram); }
		if (StaleMask & Framedash::kMemBitTextures) { HeartbeatMetrics.Remove(MemKeyTextures); }
		if (StaleMask & Framedash::kMemBitMeshes) { HeartbeatMetrics.Remove(MemKeyMeshes); }
		if (StaleMask & Framedash::kMemBitAudio) { HeartbeatMetrics.Remove(MemKeyAudio); }
		if (CachedMemMask & Framedash::kMemBitVram) { HeartbeatMetrics.FindOrAdd(MemKeyVram) = CachedMemVram; }
		if (CachedMemMask & Framedash::kMemBitTextures) { HeartbeatMetrics.FindOrAdd(MemKeyTextures) = CachedMemTextures; }
		if (CachedMemMask & Framedash::kMemBitMeshes) { HeartbeatMetrics.FindOrAdd(MemKeyMeshes) = CachedMemMeshes; }
		if (CachedMemMask & Framedash::kMemBitAudio) { HeartbeatMetrics.FindOrAdd(MemKeyAudio) = CachedMemAudio; }
		PrevMemHeartbeatMask = CachedMemMask;
	}

	if (HeartbeatMetrics.Num() > 0)
	{
		TrackWithData(TEXT("perf_heartbeat"), TEXT(""), FVector::ZeroVector,
			TMap<FString, FString>(), HeartbeatMetrics, EFramedashTelemetrySource::Automated);
	}
	else
	{
		// Absent means not collected; keep the no-source path allocation-free.
		Track(TEXT("perf_heartbeat"), TEXT(""), FVector::ZeroVector, EFramedashTelemetrySource::Automated);
	}
}

void UFramedashSubsystem::Tick(float  )
{
	// Background serialization or a request can finish between blocking waits or after
	// FlushBlocking returns. Running both isolated handoffs here closes the tracked batch
	// and clears bIsFlushing on the next frame; otherwise a late result could wedge the
	// single-flight guard forever. Both empty queues take only their lock and allocate
	// nothing.
	if (Transport.IsValid())
	{
		Transport->DrainPendingDispatches();
		Transport->DrainCompletedRequests();
	}

	// Drive the flush/heartbeat cadence from REAL wall-clock time, not the
	// engine-scaled DeltaTime argument: DeltaTime is 0 while paused (we tick
	// when paused now) and is stretched/compressed by time dilation, which would
	// stall or skew the 30s flush and 10s heartbeat. This mirrors the Godot SDK,
	// which measures Time.GetTicksUsec() instead of the scaled _Process delta.
	const double NowSeconds = FPlatformTime::Seconds();
	// LastTickSeconds is seeded to now in InitializeInternal, so this is always a
	// real elapsed delta -- no first-call branch needed.
	// FMath::Max guards a non-monotonic clock (NowSeconds < LastTickSeconds): a
	// backwards jump must not subtract from the cadence timers.
	const float RealDelta = FMath::Max(0.0f, static_cast<float>(NowSeconds - LastTickSeconds));
	LastTickSeconds = NowSeconds;

	TimeSinceLastFlush += RealDelta;
	TimeSinceLastHeartbeat += RealDelta;

	// Refresh the cached camera rotation on the game thread so each tracked event
	// (including the heartbeat below) stamps a fresh snapshot.
	if (bCaptureCameraRotation)
	{
		UpdateCachedCameraRotation();
	}

	if (TimeSinceLastHeartbeat >= FramedashConstants::HeartbeatIntervalSeconds)
	{
		TimeSinceLastHeartbeat = 0.0f;
		EmitPerformanceHeartbeat();
	}

	if (FlushPolicy.IsValid() && FlushPolicy->ShouldFlush( false, TimeSinceLastFlush))
	{
		// Reset timer unconditionally — Flush() may early-return when buffer is
		// empty, but we must not call it every frame after the interval elapses.
		TimeSinceLastFlush = 0.0f;
		Flush();
	}
}

#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
int32 UFramedashSubsystem::GetRegisteredTransportCompletionCountForTesting() const
{
	return Transport.IsValid()
		? Transport->GetRegisteredCompletionCountForTesting()
		: 0;
}

int32 UFramedashSubsystem::GetAppliedTransportCompletionCountForTesting() const
{
	return Transport.IsValid()
		? Transport->GetAppliedCompletionCountForTesting()
		: 0;
}
#endif

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
	// Count UTF-16 code units (what ingest validates), not FString elements, so the
	// clamp is correct on both 2-byte and 4-byte TCHAR builds. See FramedashStringUtil.h.
	return Framedash::TruncateToUtf16Units(Input, MaxLength);
}
