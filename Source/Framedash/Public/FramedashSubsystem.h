// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Templates/PimplPtr.h"
#include "FramedashTypes.h"
#include "FramedashSubsystem.generated.h"

class FFramedashEventBuffer;
class FFramedashTransport;
class FFramedashSessionManager;
class FFramedashPerformanceCollector;
class FFramedashSamplingPolicy;
class IPersistenceProvider;
namespace Framedash { class FFlushPolicy; }
namespace Framedash { class FMapLoadTimer; }

/**
 * Main API for the Framedash Telemetry SDK.
 * Automatically created per GameInstance. Use GetSubsystem<UFramedashSubsystem>().
 */
UCLASS()
class FRAMEDASH_API UFramedashSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UFramedashSubsystem();

	// USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override { return true; }

	// FTickableGameObject interface
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return !IsTemplate() && bInitialized; }
	// Tick even while the game is paused so the flush/heartbeat cadence keeps
	// running (a pause menu must not silently stall telemetry). The timers are
	// driven by real wall-clock time in Tick, not the engine-scaled DeltaTime.
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UFramedashSubsystem, STATGROUP_Tickables); }

	/**
	 * Initialize telemetry with explicit parameters (overrides project settings).
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void InitializeTelemetry(const FString& ApiKey = TEXT(""), const FString& EndpointUrl = TEXT(""), const FString& BuildId = TEXT(""), const FString& PlayerId = TEXT(""));

	/** Set or update the player ID at runtime (e.g. after login). */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void SetPlayerId(const FString& PlayerId);

	/**
	 * Begin an automated profiling session: tag every subsequent event with CI metadata
	 * so build-over-build performance can be compared in the dashboard and via
	 * `framedash perf-diff`. BuildId is stamped as the first-class build_id field; Branch,
	 * Commit and Scenario are attached as the ci.branch / ci.commit / ci.scenario
	 * attributes. Each call fully (re)defines the session rather than patching it: an empty
	 * BuildId clears any prior build_id override (events fall back to the configured
	 * build_id) and an omitted Branch/Commit/Scenario is absent from the new tag set --
	 * callers cannot incrementally update metadata across calls. With all arguments empty
	 * this is a no-op. Call once after InitializeTelemetry(), before the profiling run.
	 * No-op if the SDK is not initialized.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void BeginAutomatedSession(
		const FString& BuildId = TEXT(""),
		const FString& Branch = TEXT(""),
		const FString& Commit = TEXT(""),
		const FString& Scenario = TEXT(""));

	/**
	 * Begin an automated profiling session from the standard Framedash CI environment
	 * variables: FRAMEDASH_BUILD_ID, FRAMEDASH_GIT_BRANCH, FRAMEDASH_GIT_COMMIT,
	 * FRAMEDASH_TEST_SCENARIO. The planned `framedash run-profile-test` runner will export
	 * these before launching the game, so a CI integration needs only this one call. With
	 * none of the variables set this is a no-op (no override is started). No-op if not
	 * initialized.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void BeginAutomatedSessionFromEnvironment();

	/**
	 * End the automated profiling session: clear the ci.* session attributes set by
	 * BeginAutomatedSession AND drop the automated-session build_id override, so events
	 * emitted afterward carry the configured build_id again and are no longer folded into
	 * the candidate build's perf diff. Call Flush() first if you want the buffered tagged
	 * events sent before the tags are cleared.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void EndAutomatedSession();

	/**
	 * Track a custom telemetry event.
	 * @param EventName  Name of the event (e.g. "player_death", "zone_enter").
	 * @param MapId      Optional map identifier for spatial context.
	 * @param Position   World-space position where the event occurred.
	 * @param Source     Source type (Player or Automated).
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void Track(
		const FString& EventName,
		const FString& MapId = TEXT(""),
		FVector Position = FVector::ZeroVector,
		EFramedashTelemetrySource Source = EFramedashTelemetrySource::Player);

	/**
	 * Track an event with custom attributes and metrics (C++ only, TMap not supported in Blueprints by default).
	 */
	void TrackWithData(
		const FString& EventName,
		const FString& MapId,
		FVector Position,
		const TMap<FString, FString>& Attributes,
		const TMap<FString, double>& Metrics,
		EFramedashTelemetrySource Source = EFramedashTelemetrySource::Player);

	/**
	 * Manually report disk-read activity into the current perf_heartbeat window
	 * (io.read_bytes / io.read_time_ms / io.read_ops). Use this for release builds
	 * and custom loaders / VFS that the automatic IPlatformFile wrapper cannot see;
	 * it works regardless of the bTrackDiskIo setting and accumulates into the same
	 * window as the wrapper. The values are attached at the next heartbeat.
	 * A sample with ANY negative / non-finite component is dropped in full (no
	 * accumulation, no io.* activation) -- same contract as the Unity/Godot SDKs; a
	 * fully valid all-zero sample still counts as an active quiet window.
	 * No-op if the SDK is not initialized (call InitializeTelemetry() first): the IO
	 * accumulator is process-global, so feeding it before init would contaminate
	 * other GameInstances and make a later-initialized instance report io.* it never
	 * collected.
	 * @param Bytes        Bytes read since the last report.
	 * @param ReadTimeMs   Wall time spent reading, in milliseconds.
	 * @param Ops          Number of read operations.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void ReportIoSample(int64 Bytes, float ReadTimeMs, int32 Ops);

	/**
	 * Begin timing a map/level load. Records MapName and a monotonic start timestamp
	 * (FPlatformTime::Seconds -- real wall time, unaffected by pause / time dilation);
	 * call EndMapLoad() when loading completes to emit a map_load event. The loaded map
	 * name rides the attributes map as attributes["map_name"] and load_time_ms carries
	 * the elapsed time; map_id is left EMPTY (like perf_heartbeat) so this non-spatial
	 * event never lands in the spatial heatmap or the activation gate. Calling BeginMapLoad
	 * again before EndMapLoad REPLACES the pending measurement (the earlier one is
	 * discarded). Fail-safe (never throws into game code). No-op if the SDK is not
	 * initialized. GAME THREAD ONLY: the Track path reads game-thread-only perf/collector
	 * state, so a call from an async loader thread would race and is unsupported.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void BeginMapLoad(const FString& MapName);

	/**
	 * Complete the map/level load started by BeginMapLoad and emit a map_load event
	 * (attributes["map_name"] = the stored map name, metrics load_time_ms = elapsed
	 * milliseconds, map_id empty) via the normal Track path (sampling, buffering, session
	 * attributes). No-op if no BeginMapLoad is pending or the SDK is not initialized.
	 * Fail-safe. GAME THREAD ONLY (same reason as BeginMapLoad).
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void EndMapLoad();

	/**
	 * Directly report a map/level load time for developers who measure it themselves
	 * (custom loaders / streaming), bypassing the Begin/End timer. Emits the same map_load
	 * event shape (attributes["map_name"] = MapName, metrics load_time_ms = LoadTimeMs,
	 * map_id empty). A NaN / Infinity / negative LoadTimeMs is DROPPED (the whole call, not
	 * clamped), matching the manual metric-feed contract; the map name is clamped to the
	 * ingest attribute-value cap. Fail-safe. No-op if the SDK is not initialized. GAME
	 * THREAD ONLY: report from the game thread once your async load completes (a call from
	 * the loader thread would race TrackWithData + the perf collector); this is resolved by
	 * contract, not by marshaling.
	 * @param MapName     Identifier of the loaded map/level (rides attributes["map_name"]).
	 * @param LoadTimeMs  Measured load time in milliseconds.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void ReportMapLoad(const FString& MapName, double LoadTimeMs);

	/** Flush all buffered events immediately. */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void Flush();

	/**
	 * Set a per-event-name sampling rate that overrides the global rate for that event.
	 * Empty event names are ignored. Rate is clamped to [0, 1].
	 * Has no effect if the SDK is not initialized.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	void SetEventSamplingRate(const FString& EventName, float Rate);

	/**
	 * Remove a per-event-name sampling override so the event falls back to the global rate.
	 * Returns true if an override was present.
	 */
	UFUNCTION(BlueprintCallable, Category="Framedash")
	bool RemoveEventSamplingRate(const FString& EventName);

	/** Whether the SDK is initialized and ready to track events. */
	UFUNCTION(BlueprintPure, Category="Framedash")
	bool IsInitialized() const { return bInitialized; }

	/** Get the current session ID. */
	UFUNCTION(BlueprintPure, Category="Framedash")
	FString GetSessionId() const;

private:
	void InitializeInternal(const FString& ApiKey, const FString& EndpointUrl, const FString& BuildId, const FString& PlayerId, float SamplingRate);
	void ShutdownTelemetry();
	void DestroyComponents();
	uint64 TrackInFlightBatch(const TArray<FFramedashEvent>& Events, int32& OutPersistedEventCount);
	void CloseInFlightBatch(uint64 BatchId, int32 DeliveredLeadingEventCount);
	void PersistInFlightBatches();
	void PersistFailedEvents(TArray<FFramedashEvent>&& Events);
	void WaitForFailurePersistenceTasks();

	/** Truncate string to max length. */
	static FString TruncateString(const FString& Input, int32 MaxLength);

	/**
	 * Emit the map_load event via the normal Track path (sampling, buffering, session
	 * attributes). map_id stays EMPTY (keeps map_load out of spatial heatmap and
	 * activation paths); MapName rides attributes["map_name"] and the load time
	 * rides the metrics map as
	 * load_time_ms (no proto/ClickHouse field, mirroring the io.* attributes-map
	 * guardrail). A non-finite LoadTimeMs is dropped by TrackWithData's finite-metric
	 * guard, matching the drop-not-clamp rule for the direct feed.
	 */
	void TrackMapLoad(const FString& MapName, double LoadTimeMs);

	/** Refresh the cached camera rotation on the game thread (called from Tick). */
	void UpdateCachedCameraRotation();

	/** Stamp Evt.CameraYaw/CameraPitch from the thread-safe cached snapshot. */
	void CaptureCameraRotation(FFramedashEvent& Evt) const;

	// Sample memory detail (engine reads) and store it into the scalar cache
	// (CachedMemMask + per-category doubles) from the engine-independent selection.
	// Allocation-free (no map), so it is safe to call on the zero-alloc heartbeat path.
	// Called at the heartbeat cadence and lazily on the first position-qualified event.
	// Sets bMemorySampleValid. Defined in the .cpp where the Private
	// FramedashMemorySample.h / collector are visible.
	void RefreshMemorySample();

	// TPimplPtr type-erases the deleter at construction time, so forward-declared
	// (incomplete) types work without C4150 on MSVC. No out-of-line destructor needed.
	TPimplPtr<FFramedashEventBuffer> EventBuffer;
	TPimplPtr<FFramedashTransport> Transport;
	TPimplPtr<FFramedashSessionManager> SessionManager;
	TPimplPtr<FFramedashPerformanceCollector> PerformanceCollector;
	TPimplPtr<FFramedashSamplingPolicy> SamplingPolicy;
	TPimplPtr<Framedash::FFlushPolicy> FlushPolicy;
	// Pending map/level load-time measurement (BeginMapLoad/EndMapLoad). The pure timer
	// holds only the monotonic start timestamp + pending flag (engine-free, GoogleTest-
	// covered); the loaded map NAME lives beside it as an FString so the timer header
	// stays UE-free. Both are game-thread only (set/read from the public map-load API),
	// like the other cached session state. Recreated on each (re-)init.
	TPimplPtr<Framedash::FMapLoadTimer> MapLoadTimer;
	FString PendingMapLoadName;
	TSharedPtr<IPersistenceProvider, ESPMode::ThreadSafe> PersistenceProvider;
	struct FInFlightBatch
	{
		TArray<FFramedashEvent> Events;
		int32 PersistedEventCount = 0;
	};
	TMap<uint64, FInFlightBatch> InFlightBatches;
	FCriticalSection InFlightCriticalSection;
	TArray<TFuture<void>> FailurePersistenceTasks;
	FCriticalSection FailurePersistenceCriticalSection;

	FString CachedApiKey;
	FString CachedEndpointUrl;
	FString CachedBuildId;
	FString CachedPlatform;
	FString CachedEngineVersion;

	// Session-level attributes set by BeginAutomatedSession (CI metadata:
	// ci.branch / ci.commit / ci.scenario). Empty when no automated session is
	// active. Stamped onto every event in TrackWithData (before the per-event
	// attributes, which override on a key collision) so CI-tagged performance data
	// is queryable per build/branch/scenario. Game-thread only (set from the public
	// API, read in TrackWithData), like the other cached config above.
	TMap<FString, FString> SessionAttributes;

	// Automated-session (CI) build_id override set by BeginAutomatedSession. Events are
	// stamped with it in preference to CachedBuildId so the candidate build_id never has
	// to be written over CachedBuildId (which a re-init or a later config change would
	// otherwise strand). Empty = no session override, fall back to CachedBuildId.
	// EndAutomatedSession / re-init clear it. compareBuildPerformance groups by build_id.
	FString SessionBuildId;

	float TimeSinceLastFlush = 0.0f;
	float TimeSinceLastHeartbeat = 0.0f;
	// Real wall-clock timestamp (FPlatformTime::Seconds) of the previous Tick.
	// The flush/heartbeat timers accumulate this real delta instead of the
	// engine-scaled DeltaTime so pausing or time dilation does not stall or
	// stretch the cadence. Seeded to now in InitializeInternal so the first Tick
	// measures a one-frame delta (Tick has no first-call branch).
	double LastTickSeconds = 0.0;
	int32 EstimatedPayloadBytes = 0;
	int32 PendingPersistedEventsToAck = 0;
	int32 RejectedDurablePrefixEventCount = 0;
	uint64 NextBatchId = 1;
	bool bInitialized = false;
	bool bIsFlushing = false;
	bool bWarnedEmptyPlayerId = false;
	bool bOfflineQueueEnabled = true;
	bool bCaptureCameraRotation = true;

	// Persistent map for the metrics attached to perf_heartbeat (io.* window totals and
	// mem.* memory detail). Held as a member and maintained IN PLACE across heartbeats
	// -- NOT Reset()+rebuilt -- so the steady-state heartbeat allocates no heap (the
	// CLAUDE.md rule: the periodic heartbeat must stay zero heap allocation). io.* keys
	// (once active, always the same 3) use FindOrAdd so only the first heartbeat
	// allocates their keys; later heartbeats just overwrite the doubles. mem.* keys can
	// appear/disappear between heartbeats (LLM tag crossing 0, RHI availability), so
	// they are diffed against PrevMemHeartbeatMask: a key still present is overwritten
	// in place (no alloc); only a genuine key-set TRANSITION Removes a stale key or Adds
	// a new one (allocation on that rare transition is the accepted trade-off). Game-
	// thread only (Tick). Empty and untouched while no source is active, so the default
	// heartbeat keeps its zero-allocation Track() path.
	TMap<FString, double> HeartbeatMetrics;
	// mem.* key set (Framedash::kMemBit* bits) attached to HeartbeatMetrics on the last
	// heartbeat, used to diff and Remove keys that vanished. Reset in InitializeInternal.
	uint32 PrevMemHeartbeatMask = 0;

	// This subsystem's own baseline snapshot of the process-global cumulative IO
	// counters (Framedash::GlobalIoStats). The heartbeat computes its window as
	// cumulative - baseline, then advances the baseline. Kept as scalars (not the
	// Private FIoSnapshot type) so this Public header pulls in no Private header
	// for external game code. Game-thread only (Tick). With multiple simultaneous
	// GameInstances each subsystem holds an independent baseline, so every instance
	// reports the same process-wide IO for its own interval (no destructive drain).
	int64 IoBaselineBytes = 0;
	double IoBaselineTimeMs = 0.0;
	int64 IoBaselineOps = 0;

	// True when THIS subsystem requested disk-IO metering (bTrackDiskIo was set at
	// its init and Install() was called -- Install is fail-safe/void, so this does
	// NOT guarantee the wrapper actually installed). Deinitialize calls Disable()
	// only when true, balancing the ref-counted enablement so shutting one
	// GameInstance down does not stop metering for others still running (see
	// FFramedashIoTrackingPlatformFile).
	bool bTrackDiskIoInstalled = false;

	// True once a manual ReportIoSample was ACCEPTED during THIS session (set on the
	// accepted path, reset in InitializeInternal). Together with bTrackDiskIoInstalled
	// this gates the heartbeat io.* attach on SESSION-scoped state, so a later
	// telemetry session in the same process (UE Editor/PIE) with disk-IO tracking off
	// and no manual feed attaches NO io.* keys -- honoring "absent = not collected"
	// even though the process-global accumulator may have been fed by an earlier
	// session. Not the accumulator's business (it is process-wide), so tracked here.
	bool bIoManualFeedAccepted = false;

	// Session-scoped latch of the bTrackMemoryDetail setting, cached at init. Like
	// the io.* gates this is SESSION state, not process-global: a later telemetry
	// session in the same process (UE Editor/PIE) with the setting off emits NO
	// mem.* keys.
	bool bTrackMemoryDetail = false;

	// Cached mem.* sample from the last RefreshMemorySample(), refreshed at the
	// heartbeat cadence in Tick (and taken lazily on the first position-qualified event
	// before any heartbeat has fired, so the initial ~10s window is not blind). Held as
	// SCALARS (a presence bitmask + one double per category), NOT a TMap -- so
	// refreshing it allocates nothing (critical: RefreshMemorySample runs on the
	// heartbeat, which must stay zero-alloc) and this Public header pulls in no Private
	// FMemoryMetrics type, same pattern as the io.* baselines above. CachedMemMask holds
	// Framedash::kMemBit* bits; a value is meaningful only when its bit is set (absent
	// category => bit clear => key stays absent). Position-qualified events read these
	// scalars to attach mem.* in canonical order; that per-event attach may allocate
	// (allowed for a Track carrying metrics), but the heartbeat and refresh do not.
	uint32 CachedMemMask = 0;
	double CachedMemVram = 0.0;
	double CachedMemTextures = 0.0;
	double CachedMemMeshes = 0.0;
	double CachedMemAudio = 0.0;
	// True once a sample has been taken this session (heartbeat or lazy first event);
	// reset in InitializeInternal so a new session starts blind, not with stale data.
	bool bMemorySampleValid = false;

	// Camera rotation sampled on the game thread (Tick) and read in
	// CaptureCameraRotation. The lock guards the pair so it is always read coherently.
	mutable FCriticalSection CameraRotationCS;
	float CachedCameraYaw = 0.0f;
	float CachedCameraPitch = 0.0f;
	bool bHasCachedCamera = false;
};
