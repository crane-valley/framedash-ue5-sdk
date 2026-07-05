// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"

#include <atomic>

using FFramedashBatchFailureHandler = TFunction<void(TArray<FFramedashEvent>&&, int32)>;
using FFramedashBatchClosedHandler = TFunction<void(int32)>;

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
struct FFramedashPlanCache;
class ISslCertificateManager;
#endif

/**
 * Handles Protobuf serialization, gzip compression, and HTTP transport
 * for sending telemetry batches to the Framedash ingest endpoint.
 *
 * Serialization and compression run on a background thread pool to avoid
 * blocking the game thread. HTTP dispatch is posted back to the game thread.
 */
class FFramedashTransport
{
public:
	FFramedashTransport(const FString& InEndpointUrl, const FString& InApiKey);
	~FFramedashTransport();

	/**
	 * Serialize, compress, and send a batch of events via HTTP POST.
	 * CPU-heavy work (protobuf + gzip) runs on a thread pool;
	 * HTTP request is dispatched on the game thread.
	 * Takes events by value — caller should use MoveTemp().
	 */
	void SendBatch(
		TArray<FFramedashEvent> Events,
		FFramedashBatchFailureHandler OnTransientFailure = FFramedashBatchFailureHandler(),
		FFramedashBatchClosedHandler OnClosed = FFramedashBatchClosedHandler());

	// EndpointUrl and ApiKey are intentionally immutable after construction (no
	// setters). The owner (FramedashSubsystem) recreates the transport to change
	// configuration, so the endpoint cannot be swapped while a batch is in flight
	// or retrying -- this keeps the validated bEndpointSecure verdict and the URL
	// the request is actually sent to provably the same value for the transport's
	// whole lifetime.

private:
	/** Serialize events to Protobuf binary using nanopb. Pure function — thread-safe. */
	static bool SerializeToProtobuf(const TArray<FFramedashEvent>& Events, TArray<uint8>& OutBytes);

	/** Compress data using gzip. Pure function — thread-safe. */
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
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed);

	/**
	 * Shared classification tail for both delivery paths: runs the retry
	 * policy on StatusCode and performs the success / retry / split / fail
	 * bookkeeping. ResponseSnippet is a body excerpt for logs (empty for the
	 * direct-socket fallback, which only parses the status line) and
	 * bHasResponse says whether a real HTTP response backs it. Game thread
	 * only.
	 */
	void HandleResponseOutcome(
		int32 StatusCode,
		const FString& ResponseSnippet,
		bool bHasResponse,
		TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
		TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
		int32 RetryCount,
		int32 FamilyIndex,
		bool bIsCompressed,
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed);

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
	/**
	 * Lazily load the engine SSL module and decide whether the direct-socket
	 * fallback may engage (trust roots available, endpoint domain not
	 * key-pinned -- the fallback must never bypass a game's pinning config).
	 * Game thread only; the verdict is cached after the first call.
	 */
	bool EnsureSslReady();

	/**
	 * Launch the direct-socket TLS fallback for one attempt on a background
	 * thread (resolve-with-cache + pinned-family POST), then hop back to the
	 * game thread and classify its status code exactly like a primary
	 * response. Game thread only (dispatch); never blocks it.
	 */
	void StartDirectSocketFallback(
		TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
		TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
		int32 RetryCount,
		int32 FamilyIndex,
		bool bIsCompressed,
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed);
#endif

	/** Validate that the endpoint URL uses HTTPS (localhost exemption). */
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
		FFramedashBatchClosedHandler OnClosed);

	FString EndpointUrl;
	FString ApiKey;

	/**
	 * Cached result of ValidateEndpointSecurity(), computed once in the
	 * constructor. EndpointUrl is immutable after construction (no setters), so
	 * the verdict stays valid for the transport's lifetime and the security check
	 * does not re-parse / re-allocate on every batch flush on the game thread.
	 */
	bool bEndpointSecure = false;

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
	/**
	 * Whether the endpoint qualifies for the prefer-IPv4-with-IPv6-fallback
	 * direct-socket path at all (remote HTTPS hostname; loopback / IP-literal
	 * / non-HTTPS endpoints pass through). Computed once in the constructor
	 * (EndpointUrl is immutable).
	 */
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
	 * Shared flag to detect Transport destruction in async callbacks.
	 * Lambdas capture this by value (shared ownership keeps the atomic alive
	 * even after the Transport is deleted). Set to false in destructor.
	 * std::atomic<bool> for safe cross-thread reads/writes of the flag itself;
	 * ESPMode::ThreadSafe for safe cross-thread reference counting of the TSharedPtr.
	 */
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlag;
};
