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

static constexpr int32 DurablePrefixRejectionLogInterval = 100;

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

	FFramedashEvent Evt;
	Evt.EventName = MoveTemp(SafeEventName);

	const FDateTime UtcNow = FDateTime::UtcNow();
	// FDateTime ticks are 100ns intervals since Jan 1, 0001
	// Convert to Unix epoch microseconds
	const int64 UnixEpochTicks = FDateTime(1970, 1, 1).GetTicks();
	Evt.TimestampUs = (UtcNow.GetTicks() - UnixEpochTicks) / 10;

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

	if (PendingPersistedEventsToAck > 0)
	{
		// The file queue is acknowledged positionally, so its in-memory prefix must not
		// be overwritten. Flushing makes room without adding disk I/O to Track().
		if (!EventBuffer->TryEnqueuePreservingOldest(MoveTemp(Evt)))
		{
			++RejectedDurablePrefixEventCount;
			if (RejectedDurablePrefixEventCount % DurablePrefixRejectionLogInterval == 1)
			{
				UE_LOG(LogFramedash, Warning,
					TEXT("Event buffer full with a durable prefix; %d incoming event(s) rejected so far."),
					RejectedDurablePrefixEventCount);
			}
			Flush();
			return;
		}
	}
	else
	{
		EventBuffer->Enqueue(MoveTemp(Evt));
	}

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
