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
	MapLoadTimer.Reset();
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

	// Reject empty OR whitespace-only names. The empty rejection mirrors a hard
	// ingest requirement (validation.ts REQUIRED_NON_EMPTY_FIELDS checks
	// length === 0; it does NOT trim); the whitespace rejection is an SDK-side
	// data-quality guard (a whitespace name would pass ingest but is useless).
	// Matches the Godot SDK's IsNullOrWhiteSpace guard.
	// Non-allocating whitespace/empty check (TrimStartAndEnd would heap-allocate a
	// copy on this per-event path just to test emptiness).
	bool bNameIsBlank = true;
	for (int32 Index = 0; Index < EventName.Len(); ++Index)
	{
		if (!FChar::IsWhitespace(EventName[Index]))
		{
			bNameIsBlank = false;
			break;
		}
	}
	if (bNameIsBlank)
	{
		UE_LOG(LogFramedash, Warning, TEXT("EventName must not be empty or whitespace. Event dropped."));
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
	// Sanitize coordinates: ingest rejects NaN/Inf or |v| > 1e9 and drops the
	// whole batch, so one bad physics frame would lose unrelated events.
	// FVector is double-precision in UE5.
	Evt.Position = FVector(
		Framedash::FieldClamps::SanitizeCoord(Position.X),
		Framedash::FieldClamps::SanitizeCoord(Position.Y),
		Framedash::FieldClamps::SanitizeCoord(Position.Z));
	Evt.MapId = TruncateString(MapId, FramedashConstants::MaxMapIdLength);
	Evt.Fps = Perf.Fps;
	Evt.FrameTimeMs = Perf.FrameTimeMs;
	Evt.MemoryUsedBytes = Perf.MemoryUsedBytes;
	Evt.GpuTimeMs = Perf.GpuTimeMs;
	Evt.GameThreadMs = Perf.GameThreadMs;
	Evt.RenderThreadMs = Perf.RenderThreadMs;
	Evt.Source = Source;
	// Prefer the automated-session build_id (CI) when one is active; otherwise the
	// configured CachedBuildId. The session value is never written over CachedBuildId, so a
	// re-init or a later config change can never strand a candidate id.
	Evt.BuildId = SessionBuildId.IsEmpty() ? CachedBuildId : SessionBuildId;
	Evt.Platform = CachedPlatform;
	Evt.EngineVersion = CachedEngineVersion;

	// Stamp the automated-session CI metadata (set by BeginAutomatedSession, already
	// value-clamped there). SessionAttributes is empty when no session is active.
	if (Attributes.Num() == 0)
	{
		// Attribute-free event (e.g. the periodic perf_heartbeat / session_start): assign the
		// precomputed, already-clamped session attributes in one copy -- no per-attribute
		// TruncateString work. With no session active SessionAttributes is empty, so the
		// steady-state heartbeat assignment allocates nothing and stays on the zero-allocation
		// path (the hard rule). During an active CI session the heartbeat carries the ci.*
		// tags, which is the rule's allowed "per-event when a Track() carries attributes"
		// allocation: one small (<=3-entry) TMap copy taken AFTER the frame metrics are
		// sampled, so it cannot perturb the values the heartbeat reports. The value-type,
		// wire-critical FFramedashEvent cannot share a TMap reference the way the Unity/Godot
		// List-based events do, so this single copy is the minimum cost of keeping the
		// heartbeat tagged for cross-SDK parity.
		Evt.Attributes = SessionAttributes;
	}
	else
	{
		// Merge path: session attributes FIRST so they are retained if the combined set hits
		// the cap, then the per-event attributes -- a per-event key with the same name
		// overrides the session value (see the per-event loop). Session values are pre-clamped;
		// per-event keys/values are clamped here. A single oversized map entry would make the
		// consumer drop the whole flush. Empty keys are skipped (the server keys the map by
		// name). A key already present (a session attribute, or an earlier per-event key that
		// truncated to the same value) is OVERRIDDEN regardless of the cap -- TMap::Add
		// replaces without growing -- so the documented "per-event overrides session" contract
		// holds even at the cap. Only a genuinely new key is subject to MaxAttributePairs.
		// Guard on the map's actual size (not a manual counter): two distinct long keys can
		// truncate to the same key and collapse to one entry, so a counter would stop short.
		for (const auto& Pair : SessionAttributes)
		{
			if (Evt.Attributes.Num() >= FramedashConstants::MaxAttributePairs) break;
			Evt.Attributes.Add(Pair.Key, Pair.Value);
		}
		for (const auto& Pair : Attributes)
		{
			if (Pair.Key.IsEmpty()) continue;
			const FString Key = TruncateString(Pair.Key, FramedashConstants::MaxAttributeKeyLength);
			// One hash lookup: an existing key (a session attribute, or an earlier per-event
			// key that truncated to the same value) is OVERRIDDEN in place regardless of the
			// cap, so the "per-event overrides session" contract holds even at the cap; only a
			// genuinely new key is subject to MaxAttributePairs.
			if (FString* ExistingValue = Evt.Attributes.Find(Key))
			{
				*ExistingValue = TruncateString(Pair.Value, FramedashConstants::MaxAttributeValueLength);
			}
			else if (Evt.Attributes.Num() < FramedashConstants::MaxAttributePairs)
			{
				Evt.Attributes.Add(Key, TruncateString(Pair.Value, FramedashConstants::MaxAttributeValueLength));
			}
		}
	}

	// Copy caller metrics FIRST, enforcing the ingest caps client-side (count, key
	// length, finite values). Non-finite metric values are rejected by ingest, so drop
	// them; empty keys are skipped. Caller metrics take priority for the whole
	// MaxMetricPairs budget: this loop is identical to the pre-mem.* behavior, so a
	// caller that fills the cap keeps exactly the events it did before this feature
	// (the SDK-injected mem.* below only use the LEFTOVER capacity). Guard on the map's
	// actual size (see attributes above) so truncated-key collisions cannot stop the
	// loop short.
	for (const auto& Pair : Metrics)
	{
		if (Evt.Metrics.Num() >= FramedashConstants::MaxMetricPairs) break;
		if (Pair.Key.IsEmpty()) continue;
		if (!FMath::IsFinite(Pair.Value)) continue;
		Evt.Metrics.Add(TruncateString(Pair.Key, FramedashConstants::MaxMetricKeyLength), Pair.Value);
	}

	// Spatial-heatmap mem.* attach: perf_heartbeat is position-less (empty map_id), so
	// the heatmap grid queries (which key on map_id + cell bounds) never see the
	// heartbeat's mem.*. Mirror the cached memory sample onto position-qualified events
	// (non-empty map_id) so per-cell memory heatmaps have data. Opt-in
	// (bTrackMemoryDetail, default OFF) so default sessions keep the zero-alloc event
	// path untouched. Attached AFTER caller metrics: the caller ALWAYS wins -- a key the
	// caller already set is skipped (Contains), and mem.* only fill the capacity left
	// after the caller loop, so a caller that fills MaxMetricPairs drops NO metrics and
	// gains NO mem.* (same as before this feature). This mirrors the host-tested
	// SelectAttachableMemoryEntries rule. The heartbeat has an empty map_id, so it is
	// not double-stamped here -- it already carries mem.* via the Metrics argument.
	if (bTrackMemoryDetail && !Evt.MapId.IsEmpty())
	{
		// Lazily take the FIRST sample here if no heartbeat has sampled yet this
		// session, so the initial ~10s window is not blind. At most one engine read
		// per session on the event path (guarded by bMemorySampleValid); every later
		// qualifying event only reads cached doubles (no sampling, no key building).
		if (!bMemorySampleValid)
		{
			RefreshMemorySample();
		}
		// Fill the remaining slots in the CANONICAL order (vram, textures, meshes,
		// audio). Order MUST match the pure SelectAttachableMemoryEntries /
		// CollectMemoryMetricEntries contract the GoogleTest cases pin -- otherwise a
		// near-cap caller could get textures/audio while mem.vram (the highest-priority
		// key, and the one the heatmap needs most) is dropped. Caller wins: a key the
		// caller already set is skipped and does not consume capacity; a key whose bit is
		// clear in this sample stays absent. Allocation here is fine (per-event Track
		// carrying metrics).
		static const FString MemKeyVram(TEXT(FRAMEDASH_MEM_KEY_VRAM));
		static const FString MemKeyTextures(TEXT(FRAMEDASH_MEM_KEY_TEXTURES));
		static const FString MemKeyMeshes(TEXT(FRAMEDASH_MEM_KEY_MESHES));
		static const FString MemKeyAudio(TEXT(FRAMEDASH_MEM_KEY_AUDIO));
		const uint32 Bits[] = {
			Framedash::kMemBitVram, Framedash::kMemBitTextures,
			Framedash::kMemBitMeshes, Framedash::kMemBitAudio };
		const FString* const Keys[] = { &MemKeyVram, &MemKeyTextures, &MemKeyMeshes, &MemKeyAudio };
		const double Values[] = { CachedMemVram, CachedMemTextures, CachedMemMeshes, CachedMemAudio };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (Evt.Metrics.Num() >= FramedashConstants::MaxMetricPairs) break;
			if ((CachedMemMask & Bits[Index]) == 0) continue;
			if (Evt.Metrics.Contains(*Keys[Index])) continue;
			Evt.Metrics.Add(*Keys[Index], Values[Index]);
		}
	}

	if (bCaptureCameraRotation)
	{
		CaptureCameraRotation(Evt);
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

void UFramedashSubsystem::ReportIoSample(int64 Bytes, float ReadTimeMs, int32 Ops)
{
	// No-op until this subsystem is initialized, matching the other public tracking
	// APIs. The accumulator is process-global, so feeding it from an uninitialized
	// instance would contaminate sibling GameInstances -- so gate on bInitialized.
	if (!bInitialized)
	{
		UE_LOG(LogFramedash, Warning, TEXT("SDK not initialized. Call InitializeTelemetry() before ReportIoSample()."));
		return;
	}

	// Reject the WHOLE sample (no accumulation, no session accept) when any component
	// is invalid, matching the Unity/Godot manual-feed contract: a garbage call must
	// not activate io.* emission with misleading zero windows. A fully valid all-zero
	// sample still accumulates and accepts (a quiet window from a live source is a
	// real signal).
	if (Bytes < 0 || Ops < 0 || !FMath::IsFinite(ReadTimeMs) || ReadTimeMs < 0.0f)
	{
		UE_LOG(LogFramedash, Warning, TEXT("ReportIoSample: invalid sample dropped (Bytes=%lld, ReadTimeMs=%f, Ops=%d)."), Bytes, ReadTimeMs, Ops);
		return;
	}

	// Record that a manual feed was accepted THIS session so the heartbeat attaches
	// io.* keys. This is session-scoped state (not the process-global accumulator),
	// so a later untracked session in the same process does not inherit it.
	bIoManualFeedAccepted = true;

	// Feed the same process-wide window the IPlatformFile wrapper uses, so custom
	// loaders / release builds contribute to io.* on the next heartbeat. Works even
	// when bTrackDiskIo is off (no wrapper).
	Framedash::FIoStatsAccumulator& IoStats = Framedash::GlobalIoStats();
	IoStats.AddRead(static_cast<int64_t>(Bytes), static_cast<double>(ReadTimeMs));
	IoStats.AddOps(static_cast<int64_t>(Ops));
}

void UFramedashSubsystem::BeginMapLoad(const FString& MapName)
{
	// No-op until initialized, matching the other public tracking APIs (MapLoadTimer
	// is created in InitializeInternal). Fail-safe: this never throws into game code.
	if (!bInitialized || !MapLoadTimer.IsValid())
	{
		return;
	}
	// The loaded map name lives beside the pure timer (which stays UE-free). A later
	// BeginMapLoad before EndMapLoad replaces both -- the timer discards the old start,
	// so this overwrite keeps the name consistent with the measurement it belongs to.
	PendingMapLoadName = MapName;
	MapLoadTimer->Begin(FPlatformTime::Seconds());
}

void UFramedashSubsystem::EndMapLoad()
{
	if (!bInitialized || !MapLoadTimer.IsValid())
	{
		return;
	}
	double ElapsedMs = 0.0;
	// No-op when no BeginMapLoad is pending (End returns false without clearing state).
	if (!MapLoadTimer->End(FPlatformTime::Seconds(), ElapsedMs))
	{
		return;
	}
	TrackMapLoad(PendingMapLoadName, ElapsedMs);
}

void UFramedashSubsystem::ReportMapLoad(const FString& MapName, double LoadTimeMs)
{
	if (!bInitialized)
	{
		return;
	}
	// Drop (do NOT clamp) a NaN / Infinity / negative report, matching the manual
	// metric-feed contract shared with the Unity/Godot SDKs.
	if (!Framedash::FMapLoadTimer::IsValidLoadTimeMs(LoadTimeMs))
	{
		return;
	}
	TrackMapLoad(MapName, LoadTimeMs);
}

void UFramedashSubsystem::TrackMapLoad(const FString& MapName, double LoadTimeMs)
{
	// Emit through the normal Track path (Player source) so the map_load event inherits
	// sampling, field/attribute clamping (attribute-value truncation, finite-metric
	// drop), buffering, and any active CI session attributes -- it is a regular event,
	// not a heartbeat. map_id is left EMPTY (like perf_heartbeat) so this non-spatial
	// event never lands in the spatial heatmap grid query or the activation gate (both
	// key on a non-empty map_id); the loaded map name rides attributes["map_name"]
	// instead, clamped to MaxAttributeValueLength by TrackWithData. The load time rides
	// the metrics map as load_time_ms (no proto/ClickHouse field, mirroring the io.*
	// attributes-map guardrail). MaxAttributePairs / MaxMetricPairs are far above the one
	// entry each here, and TrackWithData drops a non-finite value, so no extra guard.
	// Keys are plain TCHAR literals (not function-local static FStrings): map_load is
	// rare, so the per-call FString construction is negligible, and a static FString's
	// destructor can run after the engine allocator is torn down at shutdown.
	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("map_name"), MapName);
	TMap<FString, double> Metrics;
	Metrics.Add(TEXT("load_time_ms"), LoadTimeMs);
	TrackWithData(TEXT("map_load"), TEXT(""), FVector::ZeroVector,
		Attributes, Metrics, EFramedashTelemetrySource::Player);
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
				FailedEvents.RemoveAt(0, PersistedFailureCount, FRAMEDASH_ALLOW_SHRINKING_NO);
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

		for (TFuture<void>& Task : Tasks)
		{
			Task.Wait();
		}
	}
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
		// Dedicated servers / headless have no local view to sample.
		const UWorld* World = GetWorld();
		const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		if (IsValid(PC) && IsValid(PC->PlayerCameraManager)) // IsValid: not pending-kill/GC
		{
			const FRotator ViewRotation = PC->PlayerCameraManager->GetCameraRotation();
			// Finite-only: a non-finite component clears the cache (both unset).
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

void UFramedashSubsystem::Tick(float /*DeltaTime*/)
{
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

	// Periodic performance heartbeat — auto-collects metrics without game code
	if (TimeSinceLastHeartbeat >= FramedashConstants::HeartbeatIntervalSeconds)
	{
		TimeSinceLastHeartbeat = 0.0f;

		// The persistent HeartbeatMetrics map is maintained IN PLACE (never Reset here)
		// so the steady-state heartbeat allocates no heap (CLAUDE.md rule). io.* keys are
		// overwritten via FindOrAdd; mem.* keys are diffed so only a key-set transition
		// touches the map. All keys are well under MaxMetricPairs and TrackWithData
		// clamps finiteness anyway.

		// io.* window metrics: attached ONLY when a collection source was active in
		// THIS session -- this subsystem REQUESTED metering (bTrackDiskIoInstalled --
		// Install() is fail-safe/void, so success is not guaranteed) or a manual
		// ReportIoSample was accepted this session (bIoManualFeedAccepted). Deliberately
		// NOT the accumulator's process-global state -- UE Editor/PIE runs many sessions
		// in one process, and a later session with tracking off must emit NO io.*
		// (absent = not collected, distinct from a collected 0), even though the
		// process-global counters may hold reads from an earlier session.
		if (bTrackDiskIoInstalled || bIoManualFeedAccepted)
		{
			// Non-destructive snapshot minus THIS subsystem's baseline = its window.
			// Cumulative (not drain) so simultaneous GameInstances each see the full
			// process-wide IO for their own interval instead of racing to drain it.
			const Framedash::FIoSnapshot IoNow = Framedash::GlobalIoStats().ReadCumulative();
			Framedash::FIoSnapshot IoBaseline;
			IoBaseline.ReadBytes = IoBaselineBytes;
			IoBaseline.ReadTimeMs = IoBaselineTimeMs;
			IoBaseline.ReadOps = IoBaselineOps;
			const Framedash::FIoSnapshot IoWindow = IoNow.Since(IoBaseline);
			// Advance the baseline so the next window starts here.
			IoBaselineBytes = IoNow.ReadBytes;
			IoBaselineTimeMs = IoNow.ReadTimeMs;
			IoBaselineOps = IoNow.ReadOps;
			// FindOrAdd overwrites the doubles in place: io.* keys never disappear once
			// active (they are the same 3 every heartbeat), so only the first active
			// heartbeat allocates the FString keys and steady state is zero-alloc.
			static const FString IoKeyReadBytes(TEXT("io.read_bytes"));
			static const FString IoKeyReadTimeMs(TEXT("io.read_time_ms"));
			static const FString IoKeyReadOps(TEXT("io.read_ops"));
			HeartbeatMetrics.FindOrAdd(IoKeyReadBytes) = static_cast<double>(IoWindow.ReadBytes);
			HeartbeatMetrics.FindOrAdd(IoKeyReadTimeMs) = IoWindow.ReadTimeMs;
			HeartbeatMetrics.FindOrAdd(IoKeyReadOps) = static_cast<double>(IoWindow.ReadOps);
		}

		// mem.* memory-detail metrics: session-scoped like io.* (bTrackMemoryDetail
		// latched at init). Refresh the (scalar) cache at this cadence, then update the
		// persistent map by DIFF so steady state (same key set as last heartbeat) is
		// zero-alloc: a still-present key is overwritten via FindOrAdd (no alloc), a
		// vanished key is Removed, and only a genuinely new key allocates -- the rare
		// transition cost is the deliberate trade-off vs. rebuilding every heartbeat. The
		// same scalar cache is mirrored onto position-qualified events in TrackWithData so
		// the spatial heatmap grid (which never sees the position-less heartbeat) can
		// surface per-cell memory. mem.vram works without LLM; category keys are present
		// only when LLM is enabled and the tag reports a positive amount -- an unavailable
		// source stays absent (never a fabricated 0).
		if (bTrackMemoryDetail)
		{
			RefreshMemorySample();
			static const FString MemKeyVram(TEXT(FRAMEDASH_MEM_KEY_VRAM));
			static const FString MemKeyTextures(TEXT(FRAMEDASH_MEM_KEY_TEXTURES));
			static const FString MemKeyMeshes(TEXT(FRAMEDASH_MEM_KEY_MESHES));
			static const FString MemKeyAudio(TEXT(FRAMEDASH_MEM_KEY_AUDIO));
			// Remove keys present last heartbeat but gone now (Remove does not allocate).
			const uint32 StaleMask = Framedash::StaleMemoryKeysMask(PrevMemHeartbeatMask, CachedMemMask);
			if (StaleMask & Framedash::kMemBitVram) { HeartbeatMetrics.Remove(MemKeyVram); }
			if (StaleMask & Framedash::kMemBitTextures) { HeartbeatMetrics.Remove(MemKeyTextures); }
			if (StaleMask & Framedash::kMemBitMeshes) { HeartbeatMetrics.Remove(MemKeyMeshes); }
			if (StaleMask & Framedash::kMemBitAudio) { HeartbeatMetrics.Remove(MemKeyAudio); }
			// Overwrite/insert currently-present keys (alloc only when a key is new).
			if (CachedMemMask & Framedash::kMemBitVram) { HeartbeatMetrics.FindOrAdd(MemKeyVram) = CachedMemVram; }
			if (CachedMemMask & Framedash::kMemBitTextures) { HeartbeatMetrics.FindOrAdd(MemKeyTextures) = CachedMemTextures; }
			if (CachedMemMask & Framedash::kMemBitMeshes) { HeartbeatMetrics.FindOrAdd(MemKeyMeshes) = CachedMemMeshes; }
			if (CachedMemMask & Framedash::kMemBitAudio) { HeartbeatMetrics.FindOrAdd(MemKeyAudio) = CachedMemAudio; }
			PrevMemHeartbeatMask = CachedMemMask;
		}

		if (HeartbeatMetrics.Num() > 0)
		{
			// Heartbeat-only attach -- no io.*/mem.* on regular events.
			TrackWithData(TEXT("perf_heartbeat"), TEXT(""), FVector::ZeroVector,
				TMap<FString, FString>(), HeartbeatMetrics, EFramedashTelemetrySource::Automated);
		}
		else
		{
			// No source active: attach nothing (absent = not collected) and keep the
			// steady-state heartbeat on the zero-allocation Track() path.
			Track(TEXT("perf_heartbeat"), TEXT(""), FVector::ZeroVector, EFramedashTelemetrySource::Automated);
		}
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
	// Count UTF-16 code units (what ingest validates), not FString elements, so the
	// clamp is correct on both 2-byte and 4-byte TCHAR builds. See FramedashStringUtil.h.
	return Framedash::TruncateToUtf16Units(Input, MaxLength);
}
