// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"

#include <atomic>

using FFramedashBatchFailureHandler = TFunction<void(TArray<FFramedashEvent>&&, int32)>;
using FFramedashBatchClosedHandler = TFunction<void(int32)>;

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

	/** Send an HTTP POST request with the given payload (shared to avoid copies on retry). */
	void SendHttpRequest(
		TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
		TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
		int32 RetryCount,
		bool bIsCompressed,
		FFramedashBatchFailureHandler OnTransientFailure,
		FFramedashBatchClosedHandler OnClosed);

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

	/**
	 * Shared flag to detect Transport destruction in async callbacks.
	 * Lambdas capture this by value (shared ownership keeps the atomic alive
	 * even after the Transport is deleted). Set to false in destructor.
	 * std::atomic<bool> for safe cross-thread reads/writes of the flag itself;
	 * ESPMode::ThreadSafe for safe cross-thread reference counting of the TSharedPtr.
	 */
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlag;
};
