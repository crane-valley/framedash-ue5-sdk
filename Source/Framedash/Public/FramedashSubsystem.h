// Copyright Crane Valley. All Rights Reserved.

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

	/** Refresh the cached camera rotation on the game thread (called from Tick). */
	void UpdateCachedCameraRotation();

	/** Stamp Evt.CameraYaw/CameraPitch from the thread-safe cached snapshot. */
	void CaptureCameraRotation(FFramedashEvent& Evt) const;

	// TPimplPtr type-erases the deleter at construction time, so forward-declared
	// (incomplete) types work without C4150 on MSVC. No out-of-line destructor needed.
	TPimplPtr<FFramedashEventBuffer> EventBuffer;
	TPimplPtr<FFramedashTransport> Transport;
	TPimplPtr<FFramedashSessionManager> SessionManager;
	TPimplPtr<FFramedashPerformanceCollector> PerformanceCollector;
	TPimplPtr<FFramedashSamplingPolicy> SamplingPolicy;
	TPimplPtr<Framedash::FFlushPolicy> FlushPolicy;
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
	uint64 NextBatchId = 1;
	bool bInitialized = false;
	bool bIsFlushing = false;
	bool bWarnedEmptyPlayerId = false;
	bool bOfflineQueueEnabled = true;
	bool bCaptureCameraRotation = true;

	// Camera rotation sampled on the game thread (Tick) and read in
	// CaptureCameraRotation. The lock guards the pair so it is always read coherently.
	mutable FCriticalSection CameraRotationCS;
	float CachedCameraYaw = 0.0f;
	float CachedCameraPitch = 0.0f;
	bool bHasCachedCamera = false;
};
