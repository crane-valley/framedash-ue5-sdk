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
	virtual bool IsTickableWhenPaused() const override { return false; }
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

	float TimeSinceLastFlush = 0.0f;
	float TimeSinceLastHeartbeat = 0.0f;
	int32 EstimatedPayloadBytes = 0;
	int32 PendingPersistedEventsToAck = 0;
	uint64 NextBatchId = 1;
	bool bInitialized = false;
	bool bIsFlushing = false;
	bool bWarnedEmptyPlayerId = false;
	bool bOfflineQueueEnabled = true;
};
