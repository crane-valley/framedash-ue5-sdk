// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashTransport.h"
#include "Framedash.h"
#include "FramedashBatchPolicy.h"
#include "FramedashEndpointSecurity.h"
#include "FramedashEngineCompat.h"
#include "FramedashProtobufSerializer.h"
#include "FramedashRetryPolicy.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Compression.h"
#include "Misc/EngineVersionComparison.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
// UE_VERSION_NEWER_THAN_OR_EQUAL was only added in UE 5.6; UE_VERSION_OLDER_THAN
// exists on every 5.x, so the ">= 5.4" guards below use its complement to stay
// buildable on 5.3-5.5 (BuildPlugin compiles with -WarningsAsErrors, so an
// undefined identifier in an #if is a fatal error, not a warning).
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
#include "Tasks/Task.h"
#endif

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
#include "FramedashAddressPlanner.h"
#include "FramedashDirectSocketSender.h"
#include "FramedashRawHttp.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Interfaces/ISslManager.h"
#include "Modules/ModuleManager.h"
#include "SslModule.h"
#endif

#include <atomic>
#include <string>

namespace
{

std::string ToUtf8EndpointString(const FString& Value)
{
	if (Value.IsEmpty()) return {};

	FTCHARToUTF8 Utf8(*Value);
	return std::string(Utf8.Get(), Utf8.Length());
}

int64 CountDecodedEntries(const TArray<FFramedashEvent>& Events)
{
	int64 Total = Events.Num();
	for (const auto& Evt : Events)
	{
		Total += Evt.Attributes.Num();
		Total += Evt.Metrics.Num();
	}
	return Total;
}

int64 EstimateSerializedBytes(const TArray<FFramedashEvent>& Events)
{
	// These deliberately generous estimates mirror Unity's
	// EventBaseByteEstimate, MapEntryOverheadBytes, and MetricEntryByteEstimate.
	constexpr int64 EventBaseByteEstimate = 256;
	constexpr int64 MapEntryOverheadBytes = 8;
	constexpr int64 MetricEntryByteEstimate = 16;
	// BMP scalar values from U+0800 through U+FFFF (including common CJK, Hangul,
	// and Kana, excluding surrogates) use one UTF-16 unit and three UTF-8 bytes.
	// A supplementary scalar uses two surrogate units and four UTF-8 bytes total,
	// so three bytes per unit conservatively covers both cases.
	constexpr int64 Utf8WorstCaseMultiplier = 3;

	int64 Total = 0;
	for (const auto& Event : Events)
	{
		// The base constant covers only fixed/numeric framing. Per-event string FIELDS
		// (unlike the map entries below) can legally add ~2KB UTF-8 each, so count their
		// actual lengths -- otherwise an under-cap chunk could serialize several MiB
		// inline and defeat the blocking serialize bound. Mirrors Unity/Godot.
		Total += EventBaseByteEstimate;
		Total +=
			(static_cast<int64>(Event.EventName.Len()) + Event.SessionId.Len() +
				Event.PlayerId.Len() + Event.MapId.Len() + Event.BuildId.Len() +
				Event.Platform.Len() + Event.EngineVersion.Len()) *
			Utf8WorstCaseMultiplier;
		for (const auto& Pair : Event.Attributes)
		{
			Total +=
				(static_cast<int64>(Pair.Key.Len()) + Pair.Value.Len()) *
					Utf8WorstCaseMultiplier +
				MapEntryOverheadBytes;
		}
		for (const auto& Pair : Event.Metrics)
		{
			Total +=
				static_cast<int64>(Pair.Key.Len()) * Utf8WorstCaseMultiplier +
				MetricEntryByteEstimate;
		}
	}
	return Total;
}

FFramedashBatchFailureHandler MakeOffsetFailureHandler(const FFramedashBatchFailureHandler& Parent, int32 BaseOffset)
{
	return [Parent, BaseOffset](TArray<FFramedashEvent>&& FailedEvents, int32 FailureOffset)
	{
		if (Parent)
		{
			Parent(MoveTemp(FailedEvents), BaseOffset + FailureOffset);
		}
	};
}

}


FFramedashTransport::FFramedashTransport(const FString& InEndpointUrl, const FString& InApiKey)
	: EndpointUrl(InEndpointUrl)
	, ApiKey(InApiKey)
	, AliveFlag(MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(true))
{
	// Compute the endpoint-security verdict once. EndpointUrl is immutable after
	// construction (no setters), so SendBatch reads the cached bool rather than
	// re-parsing/re-allocating on every flush.
	bEndpointSecure = ValidateEndpointSecurity();

	// Targeted hint for a common Config/DefaultGame.ini misconfiguration. An
	// unquoted URL value (e.g. EndpointUrl=https://ingest.framedash.dev/v1/events)
	// can be truncated to a bare scheme ("https:" / "http:") when read from the
	// .ini, because the value loses everything from "//" onward. The bare scheme
	// fails the HTTPS check and every batch is then silently dropped, which is
	// easy to mistake for a network or endpoint outage. Surface the actual fix.
	if (!bEndpointSecure && (EndpointUrl == TEXT("https:") || EndpointUrl == TEXT("http:")))
	{
		UE_LOG(LogFramedash, Error,
			TEXT("EndpointUrl is \"%s\" -- this looks truncated by UE .ini parsing (the value was cut at \"//\"). Quote the value in Config/DefaultGame.ini (EndpointUrl=\"https://...\") or remove the line to use the built-in default."),
			*EndpointUrl);
	}

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
	// Prefer-IPv4-with-IPv6-fallback eligibility is structural (remote HTTPS
	// hostname) and EndpointUrl is immutable, so decide once here. The plan
	// cache is shared with background fallback tasks (TSharedPtr) so a task
	// finishing after transport destruction still has valid storage.
	bFallbackEligible = bEndpointSecure &&
		Framedash::ShouldForceAddressFamily(ToUtf8EndpointString(EndpointUrl));
	if (bFallbackEligible)
	{
		PlanCache = MakeShared<FFramedashPlanCache, ESPMode::ThreadSafe>();
	}
#endif
}

FFramedashTransport::~FFramedashTransport()
{
	AliveFlag->store(false, std::memory_order_release);
}


// INVARIANT: the shared pending-children counter below is a plain int32. This is
// safe only because both isolated drains invoke every close callback on the game
// thread. If a future transport backend closes off-thread, the counter must become
// atomic.
void FFramedashTransport::SplitBatchAndResend(
	TArray<FFramedashEvent> Events,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed,
	FFramedashBatchDroppedHandler OnDropped,
	bool bSynchronous,
	double DeadlineSeconds)
{
	const int32 TotalCount = Events.Num();
	const int32 Mid = TotalCount / 2;

	TArray<FFramedashEvent> FirstHalf;
	FirstHalf.Reserve(Mid);
	for (int32 Index = 0; Index < Mid; ++Index)
	{
		FirstHalf.Add(MoveTemp(Events[Index]));
	}

	TArray<FFramedashEvent> SecondHalf;
	SecondHalf.Reserve(TotalCount - Mid);
	for (int32 Index = Mid; Index < TotalCount; ++Index)
	{
		SecondHalf.Add(MoveTemp(Events[Index]));
	}

	TSharedRef<int32, ESPMode::ThreadSafe> PendingChildren = MakeShared<int32, ESPMode::ThreadSafe>(2);
	TSharedRef<int32, ESPMode::ThreadSafe> FirstDeliveredLeading = MakeShared<int32, ESPMode::ThreadSafe>(0);
	TSharedRef<int32, ESPMode::ThreadSafe> SecondDeliveredLeading = MakeShared<int32, ESPMode::ThreadSafe>(0);

	const int32 FirstHalfCount = FirstHalf.Num();
	auto CloseParentIfDone = [PendingChildren, FirstDeliveredLeading, SecondDeliveredLeading, FirstHalfCount, OnClosed]()
	{
		--(*PendingChildren);
		if (*PendingChildren == 0 && OnClosed)
		{
			const int32 DeliveredLeading = *FirstDeliveredLeading >= FirstHalfCount
				? FirstHalfCount + *SecondDeliveredLeading
				: *FirstDeliveredLeading;
			OnClosed(DeliveredLeading);
		}
	};
	FFramedashBatchClosedHandler FirstClosed = [FirstDeliveredLeading, CloseParentIfDone](int32 DeliveredLeading)
	{
		*FirstDeliveredLeading = DeliveredLeading;
		CloseParentIfDone();
	};
	FFramedashBatchClosedHandler SecondClosed = [SecondDeliveredLeading, CloseParentIfDone](int32 DeliveredLeading)
	{
		*SecondDeliveredLeading = DeliveredLeading;
		CloseParentIfDone();
	};

	// OnDropped is passed straight through to both halves (not offset-wrapped like the
	// failure handler): it is a boolean-style non-delivery signal, so either half
	// dropping is enough for the caller to treat the whole flush as not fully delivered.
	// bSynchronous propagates so a split issued inside a blocking flush keeps its children
	// on the synchronous (game-thread) send path.
	SendBatch(
		MoveTemp(FirstHalf), MakeOffsetFailureHandler(OnTransientFailure, 0),
		FirstClosed, OnDropped, bSynchronous, DeadlineSeconds);
	SendBatch(
		MoveTemp(SecondHalf), MakeOffsetFailureHandler(OnTransientFailure, FirstHalfCount),
		SecondClosed, OnDropped, bSynchronous, DeadlineSeconds);
}


void FFramedashTransport::SendBatch(
	TArray<FFramedashEvent> Events,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed,
	FFramedashBatchDroppedHandler OnDropped,
	bool bSynchronous,
	double DeadlineSeconds)
{
	if (Events.Num() == 0)
	{
		if (OnClosed)
		{
			OnClosed(0);
		}
		return;
	}

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	UE_LOG(LogFramedash, Log, TEXT("SendBatch: %d events -> %s"), Events.Num(), *EndpointUrl);
#endif

	// The consumer rejects an over-cap batch wholesale, so a drain larger than a cap must be split
	// here, before serialization. Mirrors Unity SDK TransportLayer.SendBatch
	// BatchPolicy.ExceedsWireCaps placement.
	if (Framedash::FBatchPolicy::ExceedsWireCaps(Events.Num(), CountDecodedEntries(Events)))
	{
		UE_LOG(LogFramedash, Log, TEXT("Batch exceeds wire caps. Splitting %d events."), Events.Num());
		SplitBatchAndResend(
			MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed),
			MoveTemp(OnDropped), bSynchronous, DeadlineSeconds);
		return;
	}

	if (Events.Num() > FramedashConstants::MaxBatchSize)
	{
		UE_LOG(LogFramedash, Log, TEXT("Batch too large. Splitting %d events."), Events.Num());
		SplitBatchAndResend(
			MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed),
			MoveTemp(OnDropped), bSynchronous, DeadlineSeconds);
		return;
	}

	if (bSynchronous && Framedash::FBatchPolicy::ExceedsBlockingChunkBytes(
		Events.Num(), EstimateSerializedBytes(Events)))
	{
		UE_LOG(LogFramedash, Log,
			TEXT("Blocking batch estimate exceeds %lld bytes. Splitting %d events."),
			Framedash::FBatchPolicy::MaxBlockingChunkBytes, Events.Num());
		SplitBatchAndResend(
			MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed),
			MoveTemp(OnDropped), bSynchronous, DeadlineSeconds);
		return;
	}

	if (!bEndpointSecure)
	{
		UE_LOG(LogFramedash, Error, TEXT("Endpoint must use HTTPS (except localhost). Events dropped."));
		// Fail closed: the batch is dropped, not delivered. Surface it as a drop so a
		// synchronous FlushBlocking does not read the closure as success.
		if (OnDropped)
		{
			OnDropped(Events.Num());
		}
		if (OnClosed)
		{
			OnClosed(Events.Num());
		}
		return;
	}

	// The async cadence keeps CPU work off the game thread; its result uses the
	// transport-owned dispatch queue so FlushBlocking never needs the shared task graph.
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;
	TSharedRef<FPendingDispatchQueue, ESPMode::ThreadSafe> PendingDispatchQueueCopy =
		PendingDispatchQueue;

	auto TaskBody = [
		this,
		AliveFlagCopy,
		PendingDispatchQueueCopy,
		Events = MoveTemp(Events),
		OnTransientFailure = MoveTemp(OnTransientFailure),
		OnClosed = MoveTemp(OnClosed),
		OnDropped = MoveTemp(OnDropped),
		bSynchronous,
		DeadlineSeconds
	]() mutable
	{
		if (!AliveFlagCopy->load(std::memory_order_acquire)) return;

		auto FailAtDeadline = [&Events, &OnTransientFailure, &OnClosed]()
		{
			// Reuse the normal transient-failure/close chain so deadline exhaustion
			// preserves the durable prefix and settles the tracked BatchId exactly once.
			if (OnTransientFailure)
			{
				OnTransientFailure(MoveTemp(Events), 0);
			}
			if (OnClosed)
			{
				OnClosed(0);
			}
		};

		if (bSynchronous && FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			FailAtDeadline();
			return;
		}

		TArray<uint8> ProtoBytes;
		if (!SerializeToProtobuf(Events, ProtoBytes))
		{
			UE_LOG(LogFramedash, Error, TEXT("Protobuf serialization failed. %d events dropped."), Events.Num());
			// A serialize failure is a permanent drop: report it as such (OnDropped) in
			// addition to closing the batch. In synchronous mode we are already on the
			// game thread, so run the handlers inline instead of hopping.
			const int32 DroppedEventCount = Events.Num();
			if (bSynchronous)
			{
				if (OnDropped) { OnDropped(DroppedEventCount); }
				if (OnClosed) { OnClosed(DroppedEventCount); }
			}
			else if (OnClosed || OnDropped)
			{
				FFramedashTransport::EnqueuePendingDispatch(
					PendingDispatchQueueCopy,
					[AliveFlagCopy, OnClosed, OnDropped, DroppedEventCount]()
				{
					if (!AliveFlagCopy->load(std::memory_order_acquire)) return;
					if (OnDropped) { OnDropped(DroppedEventCount); }
					if (OnClosed) { OnClosed(DroppedEventCount); }
				});
			}
			return;
		}

		TArray<uint8> CompressedBytes;
		bool bIsCompressed = CompressGzip(ProtoBytes, CompressedBytes);
		if (!bIsCompressed)
		{
			UE_LOG(LogFramedash, Warning, TEXT("Gzip compression failed, sending uncompressed."));
			CompressedBytes = MoveTemp(ProtoBytes);
		}

		if (bSynchronous && FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			FailAtDeadline();
			return;
		}

		if (CompressedBytes.Num() > FramedashConstants::MaxPayloadBytes && Events.Num() > 1)
		{
			if (bSynchronous)
			{
				UE_LOG(LogFramedash, Log, TEXT("Payload too large. Splitting %d events."), Events.Num());
				SplitBatchAndResend(
					MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed),
					MoveTemp(OnDropped),   true, DeadlineSeconds);
			}
			else
			{
				FFramedashTransport::EnqueuePendingDispatch(
					PendingDispatchQueueCopy,
					[this, AliveFlagCopy, Events = MoveTemp(Events), OnTransientFailure,
						OnClosed, OnDropped, DeadlineSeconds]() mutable
				{
					if (!AliveFlagCopy->load(std::memory_order_acquire)) return;
					UE_LOG(LogFramedash, Log, TEXT("Payload too large. Splitting %d events."), Events.Num());
					SplitBatchAndResend(
						MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed),
						MoveTemp(OnDropped),   false, DeadlineSeconds);
				});
			}
			return;
		}

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
		UE_LOG(LogFramedash, Log, TEXT("  Payload: %d bytes (compressed=%s)"),
			CompressedBytes.Num(), bIsCompressed ? TEXT("yes") : TEXT("no"));
#endif

		auto Payload = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(CompressedBytes));
		auto OriginalEvents = MakeShared<TArray<FFramedashEvent>, ESPMode::ThreadSafe>(MoveTemp(Events));
		if (bSynchronous)
		{
			// Already on the game thread: dispatch the request directly. ProcessRequest is
			// non-blocking; the HTTP thread publishes completion for the blocking pump's
			// transport-local drain.
			SendHttpRequest(Payload, OriginalEvents,   0,   0,
				bIsCompressed,   true, MoveTemp(OnTransientFailure),
				MoveTemp(OnClosed), MoveTemp(OnDropped));
		}
		else
		{
			FFramedashTransport::EnqueuePendingDispatch(
				PendingDispatchQueueCopy,
				[this, AliveFlagCopy, Payload, OriginalEvents, bIsCompressed,
					OnTransientFailure, OnClosed, OnDropped]()
			{
				if (!AliveFlagCopy->load(std::memory_order_acquire)) return;
				SendHttpRequest(Payload, OriginalEvents,   0,   0,
					bIsCompressed,   false, OnTransientFailure, OnClosed, OnDropped);
			});
		}
	};

	if (bSynchronous)
	{
		// Inline dispatch avoids the shared task graph. The pre-split byte estimate and
		// deadline checks bound each indivisible serialize/gzip unit under the caller budget.
		TaskBody();
	}
	else
	{
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
		UE::Tasks::Launch(TEXT("Framedash::SerializeAndSend"), MoveTemp(TaskBody),
			UE::Tasks::ETaskPriority::BackgroundNormal);
#else
		Async(EAsyncExecution::ThreadPool, MoveTemp(TaskBody));
#endif
	}
}
