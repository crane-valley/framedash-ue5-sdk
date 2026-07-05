// Copyright Crane Valley. All Rights Reserved.

#include "FramedashTransport.h"
#include "Framedash.h"
#include "FramedashBatchPolicy.h"
#include "FramedashEndpointSecurity.h"
#include "FramedashProtobufSerializer.h"
#include "FramedashRetryPolicy.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Compression.h"
#include "Misc/EngineVersionComparison.h"
#include "Async/Async.h"
#include "Containers/Ticker.h" // FTSTicker / FTickerDelegate (transitively included on 5.6, not 5.3-5.5)
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

#include <limits>

namespace
{

std::string ToUtf8String(const FString& Value)
{
	if (Value.IsEmpty()) return {};

	FTCHARToUTF8 Utf8(*Value);
	return std::string(Utf8.Get(), Utf8.Length());
}

Framedash::ETelemetrySource ToSerializerSource(EFramedashTelemetrySource Source)
{
	switch (Source)
	{
	case EFramedashTelemetrySource::Player:
		return Framedash::ETelemetrySource::Player;
	case EFramedashTelemetrySource::Automated:
		return Framedash::ETelemetrySource::Automated;
	case EFramedashTelemetrySource::Unspecified:
	default:
		return Framedash::ETelemetrySource::Unspecified;
	}
}

Framedash::FTelemetryEvent ToSerializerEvent(const FFramedashEvent& Event)
{
	Framedash::FTelemetryEvent Out;

	Out.EventName = ToUtf8String(Event.EventName);
	Out.TimestampUs = Event.TimestampUs;
	Out.SessionId = ToUtf8String(Event.SessionId);
	Out.PlayerId = ToUtf8String(Event.PlayerId);
	if (!Event.Position.IsZero())
	{
		Out.Position = Framedash::FVector3{
			static_cast<float>(Event.Position.X),
			static_cast<float>(Event.Position.Y),
			static_cast<float>(Event.Position.Z),
		};
	}
	Out.MapId = ToUtf8String(Event.MapId);
	Out.Fps = Event.Fps;
	Out.FrameTimeMs = Event.FrameTimeMs;
	Out.MemoryUsedBytes = Event.MemoryUsedBytes;
	Out.GpuTimeMs = Event.GpuTimeMs;
	Out.Attributes.reserve(Event.Attributes.Num());
	for (const auto& Pair : Event.Attributes)
	{
		Out.Attributes.emplace_back(ToUtf8String(Pair.Key), ToUtf8String(Pair.Value));
	}
	Out.Metrics.reserve(Event.Metrics.Num());
	for (const auto& Pair : Event.Metrics)
	{
		Out.Metrics.emplace_back(ToUtf8String(Pair.Key), Pair.Value);
	}
	Out.Source = ToSerializerSource(Event.Source);
	Out.BuildId = ToUtf8String(Event.BuildId);
	Out.Platform = ToUtf8String(Event.Platform);
	Out.EngineVersion = ToUtf8String(Event.EngineVersion);
	if (Event.CameraYaw.IsSet())
	{
		Out.CameraYaw = Event.CameraYaw.GetValue();
	}
	if (Event.CameraPitch.IsSet())
	{
		Out.CameraPitch = Event.CameraPitch.GetValue();
	}
	Out.GameThreadMs = Event.GameThreadMs;
	Out.RenderThreadMs = Event.RenderThreadMs;

	return Out;
}

// Count decoded entries in a batch the way the ingest consumer does:
// one per event, plus one per attribute entry, plus one per metric entry.
// Mirrors BatchPolicy.CountDecodedEntries in the Unity SDK.
int64 CountDecodedEntries(const TArray<FFramedashEvent>& Events)
{
	int64 Total = Events.Num();
	for (const FFramedashEvent& Evt : Events)
	{
		Total += Evt.Attributes.Num();
		Total += Evt.Metrics.Num();
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

} // anonymous namespace

// -- Constructor ----------------------------------------------------------

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
		Framedash::ShouldForceAddressFamily(ToUtf8String(EndpointUrl));
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

// -- Private helper -------------------------------------------------------

// INVARIANT: the shared pending-children counter below is a plain int32 (not
// atomic). This is safe only because every close callback fires on the game
// thread (HTTP completion delegates and the payload-split AsyncTask both hop
// back to it). If a future transport backend invokes closes off-thread, the
// counter must become atomic.
void FFramedashTransport::SplitBatchAndResend(
	TArray<FFramedashEvent> Events,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed)
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

	// Shared counter: parent OnClosed fires only after BOTH halves have closed.
	// The DeliveredLeading semantic: report contiguous leading delivered events
	// (first-half count + second-half leading, or just first-half leading if the
	// first half was not fully delivered).
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

	SendBatch(MoveTemp(FirstHalf), MakeOffsetFailureHandler(OnTransientFailure, 0), FirstClosed);
	SendBatch(MoveTemp(SecondHalf), MakeOffsetFailureHandler(OnTransientFailure, FirstHalfCount), SecondClosed);
}

// -- Public API -----------------------------------------------------------

void FFramedashTransport::SendBatch(
	TArray<FFramedashEvent> Events,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed)
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

	// Check server per-request wire caps (event count AND decoded-entry count =
	// events + all attribute/metric map entries) before the per-flush batch-size
	// check. The consumer rejects an over-cap batch wholesale, so a drain larger
	// than a cap must be split here, before serialization. Mirrors Unity SDK
	// TransportLayer.SendBatch BatchPolicy.ExceedsWireCaps placement.
	if (Framedash::FBatchPolicy::ExceedsWireCaps(Events.Num(), CountDecodedEntries(Events)))
	{
		UE_LOG(LogFramedash, Log, TEXT("Batch exceeds wire caps. Splitting %d events."), Events.Num());
		SplitBatchAndResend(MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed));
		return;
	}

	if (Events.Num() > FramedashConstants::MaxBatchSize)
	{
		UE_LOG(LogFramedash, Log, TEXT("Batch too large. Splitting %d events."), Events.Num());
		SplitBatchAndResend(MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed));
		return;
	}

	if (!bEndpointSecure)
	{
		UE_LOG(LogFramedash, Error, TEXT("Endpoint must use HTTPS (except localhost). Events dropped."));
		if (OnClosed)
		{
			OnClosed(Events.Num());
		}
		return;
	}

	// Dispatch CPU-heavy serialization + compression to a background thread.
	// SerializeToProtobuf and CompressGzip are static (pure) functions — thread-safe.
	// HTTP dispatch is posted back to the game thread via AsyncTask.
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;

	auto TaskBody = [
		this,
		AliveFlagCopy,
		Events = MoveTemp(Events),
		OnTransientFailure = MoveTemp(OnTransientFailure),
		OnClosed = MoveTemp(OnClosed)
	]() mutable
	{
		if (!AliveFlagCopy->load(std::memory_order_acquire)) return;

		TArray<uint8> ProtoBytes;
		if (!SerializeToProtobuf(Events, ProtoBytes))
		{
			UE_LOG(LogFramedash, Error, TEXT("Protobuf serialization failed. %d events dropped."), Events.Num());
			if (OnClosed)
			{
				AsyncTask(ENamedThreads::GameThread, [OnClosed, DroppedEventCount = Events.Num()]()
				{
					OnClosed(DroppedEventCount);
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

		// If payload exceeds max size, split the batch on the game thread
		// (SendBatch/SplitBatchAndResend access members, must run on game thread).
		if (CompressedBytes.Num() > FramedashConstants::MaxPayloadBytes && Events.Num() > 1)
		{
			AsyncTask(ENamedThreads::GameThread, [this, AliveFlagCopy, Events = MoveTemp(Events), OnTransientFailure, OnClosed]() mutable
			{
				if (!AliveFlagCopy->load(std::memory_order_acquire)) return;
				UE_LOG(LogFramedash, Log, TEXT("Payload too large. Splitting %d events."), Events.Num());
				SplitBatchAndResend(MoveTemp(Events), MoveTemp(OnTransientFailure), MoveTemp(OnClosed));
			});
			return;
		}

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
		UE_LOG(LogFramedash, Log, TEXT("  Payload: %d bytes (compressed=%s)"),
			CompressedBytes.Num(), bIsCompressed ? TEXT("yes") : TEXT("no"));
#endif

		// Post HTTP request dispatch to the game thread
		// ThreadSafe mode — refcount is touched from both worker and game threads.
		auto Payload = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(CompressedBytes));
		auto OriginalEvents = MakeShared<TArray<FFramedashEvent>, ESPMode::ThreadSafe>(MoveTemp(Events));
		AsyncTask(ENamedThreads::GameThread, [this, AliveFlagCopy, Payload, OriginalEvents, bIsCompressed, OnTransientFailure, OnClosed]()
		{
			if (!AliveFlagCopy->load(std::memory_order_acquire)) return;
			SendHttpRequest(Payload, OriginalEvents, /*RetryCount*/ 0, /*FamilyIndex*/ 0, bIsCompressed, OnTransientFailure, OnClosed);
		});
	};

#if !UE_VERSION_OLDER_THAN(5, 4, 0)
	UE::Tasks::Launch(TEXT("Framedash::SerializeAndSend"), MoveTemp(TaskBody),
		UE::Tasks::ETaskPriority::BackgroundNormal);
#else
	Async(EAsyncExecution::ThreadPool, MoveTemp(TaskBody));
#endif
}

// -- Protobuf serialization -----------------------------------------------

bool FFramedashTransport::SerializeToProtobuf(const TArray<FFramedashEvent>& Events, TArray<uint8>& OutBytes)
{
	std::vector<Framedash::FTelemetryEvent> SerializerEvents;
	SerializerEvents.reserve(Events.Num());

	for (const FFramedashEvent& Event : Events)
	{
		SerializerEvents.emplace_back(ToSerializerEvent(Event));
	}

	std::vector<uint8_t> SerializedBytes;
	if (!Framedash::SerializeTelemetryBatch(SerializerEvents, SerializedBytes))
	{
		return false;
	}

	if (SerializedBytes.size() > static_cast<size_t>(std::numeric_limits<int32>::max()))
	{
		return false;
	}

	OutBytes.SetNumUninitialized(static_cast<int32>(SerializedBytes.size()));
	FMemory::Memcpy(OutBytes.GetData(), SerializedBytes.data(), SerializedBytes.size());
	return true;
}

// -- gzip compression -----------------------------------------------------

bool FFramedashTransport::CompressGzip(const TArray<uint8>& InData, TArray<uint8>& OutCompressed)
{
	// Allocate output buffer — gzip adds a header/trailer (~18 bytes), so add a margin
	// to avoid buffer overflow on incompressible data.
	int32 CompressedSize = InData.Num() + 256;
	OutCompressed.SetNumUninitialized(CompressedSize);

	if (!FCompression::CompressMemory(
		NAME_Gzip,
		OutCompressed.GetData(),
		CompressedSize,
		InData.GetData(),
		InData.Num()))
	{
		return false;
	}

	OutCompressed.SetNum(CompressedSize);
	return true;
}

// -- HTTP request ---------------------------------------------------------

void FFramedashTransport::SendHttpRequest(
	TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
	TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
	int32 RetryCount,
	int32 FamilyIndex,
	bool bIsCompressed,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed)
{
	FHttpModule& HttpModule = FHttpModule::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = HttpModule.CreateRequest();

	Request->SetURL(EndpointUrl);
	Request->SetVerb(TEXT("POST"));
	// SECURITY (verified against the UE 5.x curl backend): the platform HTTP
	// backend (libcurl) sets CURLOPT_FOLLOWLOCATION and follows 3xx redirects,
	// re-sending this X-API-Key header to the redirect target -- a credential leak
	// if a trusted endpoint is compromised/MITM'd. IHttpRequest exposes no portable
	// per-request toggle to disable following (unlike Unity's redirectLimit = 0),
	// so prevention is not possible at the SDK layer. Instead the completion
	// handler detects a cross-origin landing via GetEffectiveURL() (UE 5.4+) and
	// drops the batch without persisting (see IsCrossOriginRedirect). Realistic
	// risk stays low on the HTTPS-only default: TLS authenticates the endpoint, so
	// an off-path attacker cannot inject the redirect.
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/x-protobuf"));
	if (bIsCompressed)
	{
		Request->SetHeader(TEXT("Content-Encoding"), TEXT("gzip"));
	}
	Request->SetHeader(TEXT("X-API-Key"), ApiKey);
	Request->SetHeader(TEXT("X-SDK-Version"), FString::Printf(TEXT("%s/%s"), FRAMEDASH_SDK_NAME, FRAMEDASH_SDK_VERSION));
	Request->SetContent(*Payload);
	Request->SetTimeout(FramedashConstants::HttpTimeoutSeconds);

	// Capture AliveFlag by value — shared ownership keeps the atomic alive
	// even after this Transport instance is destroyed.
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;

	Request->OnProcessRequestComplete().BindLambda(
		[this, AliveFlagCopy, Payload, Events, RetryCount, FamilyIndex, bIsCompressed, OnTransientFailure, OnClosed]
		(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		if (!AliveFlagCopy->load(std::memory_order_acquire))
		{
			UE_LOG(LogFramedash, Warning, TEXT("Transport destroyed — dropping HTTP callback."));
			return;
		}

		// Normalize transport failure (no response, timeout, DNS error) to status 0
		// so the policy treats it as a retryable network error.
		const bool bHasResponse = bSuccess && Resp.IsValid();
		const int32 StatusCode = bHasResponse ? Resp->GetResponseCode() : 0;

#if !UE_VERSION_OLDER_THAN(5, 4, 0)
		// Defense in depth against a credential-leaking redirect. The platform HTTP
		// backend (libcurl) follows 3xx redirects (CURLOPT_FOLLOWLOCATION) and
		// re-sends this X-API-Key header to the redirect target, with no portable
		// per-request toggle to disable it (see SendHttpRequest). The effective URL
		// (after redirects) is read from the response, or from the request when the
		// redirected hop then failed without a usable response -- so a
		// redirect-then-timeout still fails closed here instead of falling through
		// to a retry/offline-replay that re-sends the key to the same target. A
		// cross-origin landing drops the batch WITHOUT persisting and logs a
		// security error instead of trusting the redirect target's response.
		// Limitations: only the FINAL effective URL is visible, so a chain that
		// leaves and returns to the configured origin is not detectable (UE exposes
		// no redirect-hop list), and the one-time header transmission the redirect
		// already performed cannot be undone. Unity prevents the follow outright
		// (redirectLimit = 0); UE has no equivalent. IHttpBase::GetEffectiveURL
		// exists only on UE 5.4+, hence the version guard.
		const FString EffectiveUrl = bHasResponse
			? Resp->GetEffectiveURL()
			: (Req.IsValid() ? Req->GetEffectiveURL() : FString());
		if (!EffectiveUrl.IsEmpty() && EndpointUrl != EffectiveUrl &&
			Framedash::IsCrossOriginRedirect(ToUtf8String(EndpointUrl), ToUtf8String(EffectiveUrl)))
		{
			UE_LOG(LogFramedash, Error,
				TEXT("Endpoint redirected across origin (%s -> %s); X-API-Key may be exposed. Dropping %d event(s) without retry."),
				*EndpointUrl, *EffectiveUrl, Events->Num());
			if (OnClosed)
			{
				OnClosed(Events->Num());
			}
			return;
		}
#endif

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
		// Prefer-IPv4-with-IPv6-fallback (parity with Unity #1218 / Godot
		// #1216): a transport-level failure (status 0: DNS/connect/TLS/
		// timeout/reset) on the primary IHttpRequest attempt triggers ONE
		// direct-socket TLS retry pinned to a resolved IP literal (IPv4
		// first) within the SAME attempt, so a broken-IPv6 host (blackholed
		// AAAA route) delivers in-flush instead of relying on the offline
		// queue and the next run. The healthy path stays IHttpRequest; only
		// the failure path pays for the fallback. IHttpRequest itself cannot
		// pin an address family (engine libcurl, no portable
		// CURLOPT_IPRESOLVE), hence the raw FSocket + engine-OpenSSL path.
		//
		// ATTEMPT-ACCOUNTING CONTRACT (same as Unity #1218): MaxRetries
		// bounds PRIMARY IHttpRequest attempts, and each transport-level
		// primary failure may add ONE fallback POST within the same attempt.
		// Worst case (total blackout, both paths timing out every attempt)
		// is 2 x MaxRetries POSTs and roughly 2 x HttpTimeoutSeconds (~20s)
		// wall time per attempt, in exchange for in-flush delivery whenever
		// either path works.
		if (StatusCode == 0 && bFallbackEligible && EnsureSslReady())
		{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
			UE_LOG(LogFramedash, Log, TEXT("Transport-level failure on primary connect; engaging direct-socket IPv4/IPv6 fallback (attempt %d)."), RetryCount + 1);
#endif
			StartDirectSocketFallback(Payload, Events, RetryCount, FamilyIndex, bIsCompressed, OnTransientFailure, OnClosed);
			return;
		}
#endif

		// Body excerpt for the warning/error logs only (the success path
		// never logs the body); extracted here while the response is in
		// scope so HandleResponseOutcome never needs the response object.
		const FString ResponseSnippet = (bHasResponse && StatusCode >= 300)
			? Resp->GetContentAsString().Left(500)
			: FString();

		HandleResponseOutcome(StatusCode, ResponseSnippet, bHasResponse,
			Payload, Events, RetryCount, FamilyIndex, bIsCompressed,
			OnTransientFailure, OnClosed);
	});

	Request->ProcessRequest();
}

// Shared classification tail for the primary IHttpRequest path and the
// direct-socket fallback path (which supplies bHasResponse = false and an
// empty snippet -- it only parses the status line). Game thread only.
void FFramedashTransport::HandleResponseOutcome(
	int32 StatusCode,
	const FString& ResponseSnippet,
	bool bHasResponse,
	TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
	TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
	int32 RetryCount,
	int32 FamilyIndex,
	bool bIsCompressed,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed)
{
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;

	// Reuse the engine-independent retry policy while preserving the
	// original events for offline persistence and reactive 413 splitting.
	const Framedash::FRetryPolicy Policy(FramedashConstants::MaxRetries);
	const Framedash::ERetryAction Action = Policy.Classify(StatusCode, RetryCount, Events->Num());
	const int32 MaxRetries = Policy.GetMaxRetries();

	switch (Action)
	{
	case Framedash::ERetryAction::Success:
	{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
		UE_LOG(LogFramedash, Log, TEXT("Batch sent successfully (HTTP %d)."), StatusCode);
#else
		UE_LOG(LogFramedash, Verbose, TEXT("Batch sent successfully (HTTP %d)."), StatusCode);
#endif
		if (OnClosed)
		{
			OnClosed(Events->Num());
		}
		return;
	}
	case Framedash::ERetryAction::Retry:
	{
		const float Delay = Policy.GetRetryDelaySeconds(RetryCount);
		if (StatusCode == 0)
		{
			UE_LOG(LogFramedash, Warning, TEXT("HTTP request failed (no response). Retrying in %.0fs (attempt %d/%d)..."),
				Delay, RetryCount + 1, MaxRetries);
		}
		else
		{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
			if (bHasResponse)
			{
				UE_LOG(LogFramedash, Warning, TEXT("HTTP %d response: %s"),
					StatusCode, *ResponseSnippet);
			}
#endif
			UE_LOG(LogFramedash, Warning, TEXT("HTTP %d. Retrying in %.0fs (attempt %d/%d)..."),
				StatusCode, Delay, RetryCount + 1, MaxRetries);
		}

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this, AliveFlagCopy, Payload, Events, RetryCount, FamilyIndex, bIsCompressed, OnTransientFailure, OnClosed](float) -> bool
			{
				if (!AliveFlagCopy->load(std::memory_order_acquire)) return false;
				SendHttpRequest(Payload, Events, RetryCount + 1, FamilyIndex, bIsCompressed, OnTransientFailure, OnClosed);
				return false; // One-shot
			}),
			Delay
		);
		return;
	}
	case Framedash::ERetryAction::SplitBatch:
	{
		// Materialize the TSharedRef<TArray> into a local TArray so the
		// helper signature (takes TArray by value) can move from it. The
		// handlers arrive by value here, but keep the copy semantics (the
		// same handlers may still be shared with a sibling half).
		TArray<FFramedashEvent> LocalEvents(MoveTemp(*Events));
		UE_LOG(LogFramedash, Log, TEXT("HTTP 413. Splitting %d events."), LocalEvents.Num());
		SplitBatchAndResend(MoveTemp(LocalEvents), OnTransientFailure, OnClosed);
		return;
	}
	case Framedash::ERetryAction::Fail:
	default:
	{
		if (StatusCode == 0 || StatusCode == 429 || StatusCode >= 500)
		{
			if (StatusCode == 0)
			{
				UE_LOG(LogFramedash, Error, TEXT("Network error - max retries reached. Persisting %d event(s) for retry."), Events->Num());
			}
			else
			{
				UE_LOG(LogFramedash, Error, TEXT("Max retries reached. Persisting %d event(s) for retry (HTTP %d)."),
					Events->Num(), StatusCode);
			}
			if (OnTransientFailure)
			{
				OnTransientFailure(TArray<FFramedashEvent>(*Events), 0);
			}
			if (OnClosed)
			{
				OnClosed(0);
			}
			return;
		}
		else if (StatusCode == 413)
		{
			UE_LOG(LogFramedash, Error, TEXT("HTTP 413 - single event cannot be split, event dropped."));
		}
		else
		{
			// The direct-socket fallback only parses the status line (all
			// the classification needs), so it has no body text to show.
			UE_LOG(LogFramedash, Error, TEXT("HTTP %d - events dropped. Response: %s"),
				StatusCode, bHasResponse ? *ResponseSnippet.Left(200) : TEXT("(direct-socket fallback; no response body captured)"));
		}
		if (OnClosed)
		{
			OnClosed(Events->Num());
		}
		return;
	}
	}
}

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS

// -- Direct-socket IPv4/IPv6 fallback ---------------------------------------

bool FFramedashTransport::EnsureSslReady()
{
	if (bSslInitAttempted)
	{
		return bSslReady;
	}
	bSslInitAttempted = true;

	// Module loading must happen on the game thread (all completion
	// delegates run there). LoadModulePtr instead of LoadModuleChecked:
	// a missing module disables the fallback, never asserts (fail-safe SDK).
	FSslModule* SslModule = FModuleManager::LoadModulePtr<FSslModule>("SSL");
	if (SslModule == nullptr)
	{
		UE_LOG(LogFramedash, Log, TEXT("SSL module unavailable; direct-socket IPv4/IPv6 fallback disabled (IHttpRequest path unchanged)."));
		return false;
	}

	// Refcounted engine-global init (a no-op outside monolithic builds,
	// where OpenSSL 1.1+ self-initializes). The matching ShutdownSsl is
	// deliberately never called -- see the SslManager member doc.
	SslModule->GetSslManager().InitializeSsl();

	ISslCertificateManager& CertManagerRef = SslModule->GetCertificateManager();
	if (!CertManagerRef.HasCertificatesAvailable())
	{
		// No trust roots means the fallback could not validate the peer.
		// Fail closed: keep the engine HTTP path only.
		UE_LOG(LogFramedash, Log, TEXT("No SSL trust roots available; direct-socket IPv4/IPv6 fallback disabled."));
		return false;
	}

	// If the game pinned public keys for the ingest domain, the engine HTTP
	// stack enforces them via the certificate manager. The direct-socket
	// path performs standard chain + hostname validation but does NOT
	// implement pinning -- so rather than silently weakening a configured
	// pin, disable the fallback for pinned domains entirely.
	const std::string HostUtf8 = Framedash::ExtractUrlHost(ToUtf8String(EndpointUrl));
	const FString Host(UTF8_TO_TCHAR(HostUtf8.c_str()));
	if (CertManagerRef.HasPinnedPublicKeys() && CertManagerRef.IsDomainPinned(Host))
	{
		UE_LOG(LogFramedash, Log, TEXT("Endpoint domain has pinned public keys; direct-socket IPv4/IPv6 fallback disabled (pinning is not bypassed)."));
		return false;
	}

	CertificateManager = &SslModule->GetCertificateManager();
	bSslReady = true;
	return true;
}

void FFramedashTransport::StartDirectSocketFallback(
	TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
	TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
	int32 RetryCount,
	int32 FamilyIndex,
	bool bIsCompressed,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed)
{
	// Everything the background task needs is COPIED here: the task must
	// never dereference `this` off the game thread (the transport may be
	// destroyed while it runs; only the game-thread continuation below may
	// touch `this`, after re-checking AliveFlag -- destruction also happens
	// on the game thread, so the check-then-use there cannot interleave).
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;
	TSharedRef<FFramedashPlanCache, ESPMode::ThreadSafe> Cache = PlanCache.ToSharedRef();
	ISslCertificateManager* Manager = CertificateManager; // module-owned, outlives the transport
	const FString Endpoint = EndpointUrl;
	const std::string ApiKeyUtf8 = ToUtf8String(ApiKey);
	const std::string SdkVersionUtf8 =
		ToUtf8String(FString::Printf(TEXT("%s/%s"), FRAMEDASH_SDK_NAME, FRAMEDASH_SDK_VERSION));

	auto TaskBody = [this, AliveFlagCopy, Cache, Manager, Endpoint, ApiKeyUtf8, SdkVersionUtf8,
		Payload, Events, RetryCount, FamilyIndex, bIsCompressed, OnTransientFailure, OnClosed]() mutable
	{
		int32 FallbackStatus = 0;
		int32 NextIndex = FamilyIndex;

		// Resolve (cached after the first success) and POST over a socket
		// pinned to the selected family's IP literal. AcquirePlan returns
		// null when the fallback must not engage (resolution failure or a
		// resolve already in flight); the attempt then classifies as the
		// original transport-level failure (status 0).
		TSharedPtr<const Framedash::FAddressPlan, ESPMode::ThreadSafe> Plan =
			FramedashDirectSocket::AcquirePlan(Endpoint, Cache);
		if (!Plan.IsValid() || Plan->IsPassthrough())
		{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
			UE_LOG(LogFramedash, Log, TEXT("Direct-socket fallback skipped: no delivery plan (DNS resolution pending/failed or passthrough endpoint)."));
#else
			UE_LOG(LogFramedash, Verbose, TEXT("Direct-socket fallback skipped: no delivery plan (DNS resolution pending/failed or passthrough endpoint)."));
#endif
		}
		if (Plan.IsValid() && !Plan->IsPassthrough())
		{
			const int32 AttemptCount = static_cast<int32>(Plan->Attempts.size());
			// Normalize the inherited index against the (fixed) plan size.
			const int32 Index = (FamilyIndex >= 0 && FamilyIndex < AttemptCount) ? FamilyIndex : 0;
			const std::string Head = Framedash::BuildPostHead(
				Plan->RequestTarget, Plan->HostHeader, ApiKeyUtf8, SdkVersionUtf8,
				static_cast<long long>(Payload->Num()), bIsCompressed);
			FallbackStatus = FramedashDirectSocket::PostBlocking(
				*Plan, Index, Head, *Payload,
				FramedashConstants::HttpTimeoutSeconds, *Manager);

			// The fallback itself failed at the transport level: TOGGLE to
			// the other family for the next attempt (IPv4 <-> IPv6,
			// wrapping), so an IPv6-only network delivers over IPv6 AND a
			// broken-IPv6 network returns to the working IPv4 after a
			// transient glitch instead of wedging on the IPv6 blackhole. A
			// real HTTP status keeps the same family.
			NextIndex = (FallbackStatus == 0)
				? Framedash::NextFamilyIndex(Index, AttemptCount)
				: Index;
		}

		AsyncTask(ENamedThreads::GameThread,
			[this, AliveFlagCopy, Payload, Events, RetryCount, NextIndex, bIsCompressed,
			OnTransientFailure, OnClosed, FallbackStatus]()
		{
			if (!AliveFlagCopy->load(std::memory_order_acquire))
			{
				UE_LOG(LogFramedash, Warning, TEXT("Transport destroyed - dropping direct-socket fallback result."));
				return;
			}
			HandleResponseOutcome(FallbackStatus, FString(), /*bHasResponse*/ false,
				Payload, Events, RetryCount, NextIndex, bIsCompressed,
				OnTransientFailure, OnClosed);
		});
	};

	// Same background-execution infrastructure as the serialize path.
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
	UE::Tasks::Launch(TEXT("Framedash::DirectSocketFallback"), MoveTemp(TaskBody),
		UE::Tasks::ETaskPriority::BackgroundNormal);
#else
	Async(EAsyncExecution::ThreadPool, MoveTemp(TaskBody));
#endif
}

#endif // FRAMEDASH_WITH_DIRECT_SOCKET_TLS

// -- Security validation --------------------------------------------------

bool FFramedashTransport::ValidateEndpointSecurity() const
{
	// A control character (including an embedded NUL) makes the URL malformed and
	// hostile: ToUtf8String converts via a NUL-terminated C string, so a NUL would
	// truncate the validated copy while the HTTP layer may still see the full
	// FString -- a parser differential. Reject such URLs outright, before convert.
	for (const TCHAR Ch : EndpointUrl)
	{
		if (Ch < TEXT(' ') || Ch == TCHAR(0x7f))
		{
			return false;
		}
	}

	// Delegate to the engine-independent, unit-tested check. It parses the host
	// and matches loopback exactly; an unanchored Contains() would accept hostile
	// URLs such as "http://localhost.attacker.com" or "http://evil/?localhost".
	return Framedash::IsEndpointSecure(ToUtf8String(EndpointUrl));
}
