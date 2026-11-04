// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"

#include <atomic>

using FFramedashBatchFailureHandler = TFunction<void(TArray<FFramedashEvent>&&, int32)>;
using FFramedashBatchClosedHandler = TFunction<void(int32)>;
// Invoked when a batch is permanently dropped WITHOUT delivery and WITHOUT being
// queued for retry: an insecure endpoint, a serialization failure, a cross-origin
// redirect, or a non-retryable/poison HTTP status (e.g. 401 invalid key, 4xx, or a
// 413 that cannot be split). This is distinct from OnTransientFailure (events
// persisted for retry) and from OnClosed on the success path -- both success and a
// poison-drop otherwise close with the full event count, so a caller cannot tell them
// apart from OnClosed alone. A synchronous FlushBlocking uses this to report that
// delivery was NOT confirmed even though the batch closed. It does NOT change the
// poison-drop policy: the events are still dropped, this only surfaces that fact.
using FFramedashBatchDroppedHandler = TFunction<void(int32 DroppedCount)>;

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
struct FFramedashPlanCache;
class ISslCertificateManager;
#endif

/**
 * Normal serialization and compression run on a background thread pool to avoid blocking the
 * game thread. Their result crosses through a transport-owned queue.
 */
class FFramedashTransport
{
public:
	FFramedashTransport(const FString& InEndpointUrl, const FString& InApiKey);
	~FFramedashTransport();

	// bSynchronous: when true the serialize + gzip + HTTP-dispatch runs INLINE on the
	// calling (game) thread instead of using the background pool and pending-dispatch
	// queue. Every HTTP completion is isolated onto the HTTP thread and recorded for
	// DrainCompletedRequests(), regardless of this outbound-dispatch choice, so a blocked
	// game thread never drains shared engine queues. The HTTP round-trip itself remains
	// asynchronous. Scheduled retries remain ticker-driven and are not driven by the pump.
	void SendBatch(
		TArray<FFramedashEvent> Events,
		FFramedashBatchFailureHandler OnTransientFailure = FFramedashBatchFailureHandler(),
		FFramedashBatchClosedHandler OnClosed = FFramedashBatchClosedHandler(),
		FFramedashBatchDroppedHandler OnDropped = FFramedashBatchDroppedHandler(),
		bool bSynchronous = false,
		double DeadlineSeconds = TNumericLimits<double>::Max());

	void DrainPendingDispatches();

	void DrainCompletedRequests();

#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
	// The dead-loopback spec cannot assert delivery, so these counters prove that the
	// async outbound path registers with and is applied by the isolated queue.
	int32 GetRegisteredCompletionCountForTesting() const
	{
		return RegisteredCompletionCountForTesting;
	}
	int32 GetAppliedCompletionCountForTesting() const
	{
		return AppliedCompletionCountForTesting;
	}
#endif

	// EndpointUrl and ApiKey are intentionally immutable after construction (no
	// setters). The owner (FramedashSubsystem) recreates the transport to change
	// configuration, so the endpoint cannot be swapped while a batch is in flight
	// or retrying -- this keeps the validated bEndpointSecure verdict and the URL
	// the request is actually sent to provably the same value for the transport's
	// whole lifetime.

	/**
	 * Whether sending is enabled: the cached endpoint-security verdict. False means
	 * every batch is dropped (fail closed), so a synchronous FlushBlocking must
	 * report failure immediately instead of pumping for a delivery that can never
	 * happen.
	 */
	bool IsEndpointSecure() const { return bEndpointSecure; }

private:
	static bool SerializeToProtobuf(const TArray<FFramedashEvent>& Events, TArray<uint8>& OutBytes);

	static bool CompressGzip(const TArray<uint8>& InData, TArray<uint8>& OutCompressed);

	/**
	 * Send an HTTP POST request with the given payload (shared to avoid copies
	 * on retry). FamilyIndex selects the direct-socket fallback's address
	 * family for THIS attempt (index into the cached FAddressPlan attempts,
	 * IPv4 first); it is threaded through the retry chain so a fallback that
	 * failed at the transport level toggles family on the next attempt.
	 */
	void SendHttpRequest(
		TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
		TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
		int32 RetryCount,
		int32 FamilyIndex,
		bool bIsCompressed,
		bool bSynchronous,
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed,
		FFramedashBatchDroppedHandler OnDropped = FFramedashBatchDroppedHandler());

	void HandleResponseOutcome(
		int32 StatusCode,
		const FString& ResponseSnippet,
		bool bHasResponse,
		TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
		TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
		int32 RetryCount,
		int32 FamilyIndex,
		bool bIsCompressed,
		bool bSynchronous,
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed,
		FFramedashBatchDroppedHandler OnDropped = FFramedashBatchDroppedHandler());

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
	/**
	 * The direct-socket fallback must have trust roots and must not bypass a game's endpoint
	 * pinning configuration.
	 */
	bool EnsureSslReady();

	void StartDirectSocketFallback(
		TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
		TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
		int32 RetryCount,
		int32 FamilyIndex,
		bool bIsCompressed,
		bool bSynchronous,
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed,
		FFramedashBatchDroppedHandler OnDropped = FFramedashBatchDroppedHandler());
#endif

	bool ValidateEndpointSecurity() const;

	/**
	 * Split a batch in half and re-send each half via SendBatch.
	 * Owns the shared PendingChildren counter and the CloseParentIfDone
	 * chaining so callers do not duplicate the ~50-line bookkeeping.
	 * Used by three split sites: MaxBatchSize pre-check, payload-size
	 * post-compression check, and 413 reactive split.
	 */
	void SplitBatchAndResend(
		TArray<FFramedashEvent> Events,
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed,
		FFramedashBatchDroppedHandler OnDropped = FFramedashBatchDroppedHandler(),
		bool bSynchronous = false,
		double DeadlineSeconds = TNumericLimits<double>::Max());

	struct FPendingDispatchQueue
	{
		TArray<TFunction<void()>> Dispatches;
		FCriticalSection CS;
	};

	/**
	 * Keep background results out of the process-global task graph without requiring the
	 * transport itself to outlive the producer.
	 */
	static void EnqueuePendingDispatch(
		const TSharedRef<FPendingDispatchQueue, ESPMode::ThreadSafe>& Queue,
		TFunction<void()> Dispatch);

	struct FPendingCompletion
	{
		std::atomic<bool> bComplete{false};

		int32 StatusCode = 0;
		int32 NextFamilyIndex = 0;
		bool bHasResponse = false;
		FString ResponseSnippet;
		FString EffectiveUrl;

		TFunction<void(FPendingCompletion&)> Continue;
	};

	FString EndpointUrl;
	FString ApiKey;

	// A worker may finish between the destructor's AliveFlag store and its enqueue. Shared
	// ownership keeps only the inert queue alive, so that race never dereferences `this`.
	TSharedRef<FPendingDispatchQueue, ESPMode::ThreadSafe> PendingDispatchQueue =
		MakeShared<FPendingDispatchQueue, ESPMode::ThreadSafe>();

	TArray<TSharedRef<FPendingCompletion, ESPMode::ThreadSafe>> PendingCompletions;
	FCriticalSection PendingCompletionsCS;

#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
	int32 RegisteredCompletionCountForTesting = 0;
	int32 AppliedCompletionCountForTesting = 0;
#endif

	/**
	 * Cached result of ValidateEndpointSecurity(), computed once in the
	 * constructor. EndpointUrl is immutable after construction (no setters), so
	 * the verdict stays valid for the transport's lifetime and the security check
	 * does not re-parse / re-allocate on every batch flush on the game thread.
	 */
	bool bEndpointSecure = false;

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
	bool bFallbackEligible = false;

	/** One-shot EnsureSslReady latch + verdict (game thread only). */
	bool bSslInitAttempted = false;
	bool bSslReady = false;

	/**
	 * Engine SSL certificate manager (trust roots), valid once bSslReady.
	 * Module-owned; safe to use from background fallback tasks after the
	 * game-thread load in EnsureSslReady. The matching ShutdownSsl for the
	 * InitializeSsl call is deliberately never made: a background task may
	 * still be inside OpenSSL when this transport is destroyed, and tearing
	 * global SSL state down under it would risk a crash for telemetry's
	 * sake (the InitializeSsl refcount stays +1).
	 */
	ISslCertificateManager* CertificateManager = nullptr;

	/**
	 * Shared resolve/plan cache for the fallback (see FFramedashPlanCache).
	 * Held by TSharedPtr so an in-flight background task keeps it alive past
	 * transport destruction. Created in the constructor when eligible.
	 */
	TSharedPtr<FFramedashPlanCache, ESPMode::ThreadSafe> PlanCache;
#endif

	/**
	 * Callbacks can outlive Transport. Shared ownership keeps the alive flag valid; atomic access
	 * and thread-safe reference counting protect its two separate cross-thread lifetimes.
	 */
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlag;
};
