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
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

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

}

void FFramedashTransport::EnqueuePendingDispatch(
	const TSharedRef<FPendingDispatchQueue, ESPMode::ThreadSafe>& Queue,
	TFunction<void()> Dispatch)
{
	if (!Dispatch)
	{
		return;
	}

	FScopeLock Lock(&Queue->CS);
	Queue->Dispatches.Add(MoveTemp(Dispatch));
}

void FFramedashTransport::DrainPendingDispatches()
{
	TArray<TFunction<void()>> Dispatches;
	{
		FScopeLock Lock(&PendingDispatchQueue->CS);
		if (PendingDispatchQueue->Dispatches.Num() == 0)
		{
			return;
		}

		Dispatches = MoveTemp(PendingDispatchQueue->Dispatches);
	}

	// Dispatches can enqueue later transport work. Running with the producer lock free
	// prevents self-deadlock and lets background serialization publish the next closure.
	for (auto&& Dispatch : Dispatches)
	{
		if (Dispatch)
		{
			Dispatch();
		}
	}
}

void FFramedashTransport::DrainCompletedRequests()
{
	TArray<TSharedRef<FPendingCompletion, ESPMode::ThreadSafe>> Completed;
	{
		FScopeLock Lock(&PendingCompletionsCS);
		if (PendingCompletions.Num() == 0)
		{
			return;
		}

		int32 Index = 0;
		while (Index < PendingCompletions.Num())
		{
			const auto& Entry = PendingCompletions[Index];
			if (!Entry->bComplete.load(std::memory_order_acquire))
			{
				++Index;
				continue;
			}

			Completed.Add(Entry);
			PendingCompletions.RemoveAt(
				Index, 1, FRAMEDASH_ALLOW_SHRINKING_NO);
		}
	}

	// Continue may synchronously register split children or fallback work. Keeping the
	// queue lock out of that call chain prevents self-deadlock and preserves game-thread
	// ownership of every transport and subsystem mutation.
	for (const auto& Entry : Completed)
	{
		if (Entry->Continue)
		{
#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
			++AppliedCompletionCountForTesting;
#endif
			Entry->Continue(*Entry);
		}
	}
}


bool FFramedashTransport::SerializeToProtobuf(const TArray<FFramedashEvent>& Events, TArray<uint8>& OutBytes)
{
	std::vector<Framedash::FTelemetryEvent> SerializerEvents;
	SerializerEvents.reserve(Events.Num());

	for (const auto& Event : Events)
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


void FFramedashTransport::SendHttpRequest(
	TSharedRef<TArray<uint8>, ESPMode::ThreadSafe> Payload,
	TSharedRef<TArray<FFramedashEvent>, ESPMode::ThreadSafe> Events,
	int32 RetryCount,
	int32 FamilyIndex,
	bool bIsCompressed,
	bool bSynchronous,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed,
	FFramedashBatchDroppedHandler OnDropped)
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

	// Capture AliveFlag by value - shared ownership keeps the atomic alive
	// even after this Transport instance is destroyed.
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;

	TSharedRef<FPendingCompletion, ESPMode::ThreadSafe> Pending =
		MakeShared<FPendingCompletion, ESPMode::ThreadSafe>();

	Pending->Continue =
		[this, AliveFlagCopy, Payload, Events, RetryCount, FamilyIndex, bIsCompressed,
			bSynchronous, OnTransientFailure, OnClosed, OnDropped]
		(FPendingCompletion& Completion)
	{
		if (!AliveFlagCopy->load(std::memory_order_acquire))
		{
			UE_LOG(LogFramedash, Warning, TEXT("Transport destroyed - dropping isolated HTTP completion."));
			return;
		}

#if !UE_VERSION_OLDER_THAN(5, 4, 0)
		// Keep the security decision on the game thread. The completion thread only
		// copies the request-owned effective URL into the release/acquire snapshot.
		// This preserves the previous async-path fail-closed behavior for a redirect
		// whose final hop times out without a usable response.
		if (!Completion.EffectiveUrl.IsEmpty() && EndpointUrl != Completion.EffectiveUrl &&
			Framedash::IsCrossOriginRedirect(
				ToUtf8String(EndpointUrl), ToUtf8String(Completion.EffectiveUrl)))
		{
			UE_LOG(LogFramedash, Error,
				TEXT("Endpoint redirected across origin (%s -> %s); X-API-Key may be exposed. Dropping %d event(s) without retry."),
				*EndpointUrl, *Completion.EffectiveUrl, Events->Num());
			if (OnDropped)
			{
				OnDropped(Events->Num());
			}
			if (OnClosed)
			{
				OnClosed(Events->Num());
			}
			return;
		}
#endif

#if FRAMEDASH_WITH_DIRECT_SOCKET_TLS
		if (Completion.StatusCode == 0 && bFallbackEligible && EnsureSslReady())
		{
#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
			UE_LOG(LogFramedash, Log, TEXT("Transport-level failure on primary connect; engaging direct-socket IPv4/IPv6 fallback (attempt %d)."), RetryCount + 1);
#endif
			StartDirectSocketFallback(
				Payload, Events, RetryCount, FamilyIndex, bIsCompressed, bSynchronous,
				OnTransientFailure, OnClosed, OnDropped);
			return;
		}
#endif

		HandleResponseOutcome(
			Completion.StatusCode, Completion.ResponseSnippet, Completion.bHasResponse,
			Payload, Events, RetryCount, FamilyIndex, bIsCompressed, bSynchronous,
			OnTransientFailure, OnClosed, OnDropped);
	};

	// Verified on the supported UE 5.3-5.8 floor: every IHttpRequest exposes this
	// policy, and CompleteOnHttpThread runs FinishRequest directly on the HTTP thread
	// instead of entering FHttpManager's process-global game-thread completion queue.
	// Every Framedash request must use one policy so FlushBlocking can observe work
	// dispatched by the async cadence without ticking the shared queue.
	Request->SetDelegateThreadPolicy(
		EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
	Request->OnProcessRequestComplete().BindLambda(
		[AliveFlagCopy, Pending](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		if (!AliveFlagCopy->load(std::memory_order_acquire))
		{
			UE_LOG(LogFramedash, Warning, TEXT("Transport destroyed - dropping isolated HTTP callback."));
			return;
		}

		// No transport or subsystem state is safe to mutate on the completion thread.
		// The release store publishes only request-owned response snapshots; the
		// game-thread drain performs the complete existing classification tail.
		Pending->bHasResponse = bSuccess && Resp.IsValid();
		Pending->StatusCode =
			Pending->bHasResponse ? Resp->GetResponseCode() : 0;
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
		Pending->EffectiveUrl = Pending->bHasResponse
			? Resp->GetEffectiveURL()
			: (Req.IsValid() ? Req->GetEffectiveURL() : FString());
#endif
		Pending->ResponseSnippet =
			(Pending->bHasResponse && Pending->StatusCode >= 300)
			? Resp->GetContentAsString().Left(500)
			: FString();
		Pending->bComplete.store(true, std::memory_order_release);
	});

	{
		FScopeLock Lock(&PendingCompletionsCS);
		PendingCompletions.Add(Pending);
#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
		++RegisteredCompletionCountForTesting;
#endif
	}

	Request->ProcessRequest();
}

void FFramedashTransport::HandleResponseOutcome(
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
	FFramedashBatchDroppedHandler OnDropped)
{
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;

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
			FTickerDelegate::CreateLambda([this, AliveFlagCopy, Payload, Events, RetryCount, FamilyIndex, bIsCompressed, bSynchronous, OnTransientFailure, OnClosed, OnDropped](float) -> bool
			{
				if (!AliveFlagCopy->load(std::memory_order_acquire)) return false;
				SendHttpRequest(
					Payload, Events, RetryCount + 1, FamilyIndex, bIsCompressed,
					bSynchronous, OnTransientFailure, OnClosed, OnDropped);
				return false;
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
		// A reactive 413 split re-dispatches asynchronously (the blocking pump does not
		// drive it); a blocking flush waiting on this batch resolves at its deadline with
		// the events retained. 413 on an already batch-capped payload is rare.
		SplitBatchAndResend(MoveTemp(LocalEvents), OnTransientFailure, OnClosed, OnDropped,   false);
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
		// Reached only by the permanent-drop statuses above (413-single or a non-retryable
		// 4xx such as 401 invalid key). The transient branch returned earlier via
		// OnTransientFailure. These events are dropped, not delivered, so surface the drop
		// -- a synchronous FlushBlocking must NOT read this closure as success. The
		// poison-drop policy is unchanged; only its visibility is added.
		if (OnDropped)
		{
			OnDropped(Events->Num());
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
	bool bSynchronous,
	FFramedashBatchFailureHandler OnTransientFailure,
	FFramedashBatchClosedHandler OnClosed,
	FFramedashBatchDroppedHandler OnDropped)
{
	// Everything the background task needs is COPIED here: the task must
	// never dereference `this` off the game thread (the transport may be
	// destroyed while it runs; only the game-thread continuation below may
	// touch `this`, after re-checking AliveFlag -- destruction also happens
	// on the game thread, so the check-then-use there cannot interleave).
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;
	TSharedRef<FFramedashPlanCache, ESPMode::ThreadSafe> Cache = PlanCache.ToSharedRef();
	ISslCertificateManager* Manager = CertificateManager;
	const FString Endpoint = EndpointUrl;
	const std::string ApiKeyUtf8 = ToUtf8String(ApiKey);
	const std::string SdkVersionUtf8 =
		ToUtf8String(FString::Printf(TEXT("%s/%s"), FRAMEDASH_SDK_NAME, FRAMEDASH_SDK_VERSION));

	TSharedRef<FPendingCompletion, ESPMode::ThreadSafe> Pending =
		MakeShared<FPendingCompletion, ESPMode::ThreadSafe>();
	Pending->Continue =
		[this, AliveFlagCopy, Payload, Events, RetryCount, bIsCompressed,
			bSynchronous, OnTransientFailure, OnClosed, OnDropped]
		(FPendingCompletion& Completion)
	{
		if (!AliveFlagCopy->load(std::memory_order_acquire))
		{
			UE_LOG(LogFramedash, Warning, TEXT("Transport destroyed - dropping isolated direct-socket completion."));
			return;
		}
		HandleResponseOutcome(
			Completion.StatusCode, FString(),   false,
			Payload, Events, RetryCount, Completion.NextFamilyIndex, bIsCompressed,
			bSynchronous, OnTransientFailure, OnClosed, OnDropped);
	};

	{
		FScopeLock Lock(&PendingCompletionsCS);
		PendingCompletions.Add(Pending);
#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
		++RegisteredCompletionCountForTesting;
#endif
	}

	auto TaskBody = [AliveFlagCopy, Cache, Manager, Endpoint, ApiKeyUtf8, SdkVersionUtf8,
		Payload, FamilyIndex, bIsCompressed, Pending]() mutable
	{
		int32 FallbackStatus = 0;
		int32 NextIndex = FamilyIndex;

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

		if (!AliveFlagCopy->load(std::memory_order_acquire))
		{
			UE_LOG(LogFramedash, Warning, TEXT("Transport destroyed - dropping isolated direct-socket callback."));
			return;
		}

		// A fallback result must never depend on the game-thread task graph. Uniform
		// publication lets a blocking wait observe fallback work started by async cadence.
		Pending->StatusCode = FallbackStatus;
		Pending->NextFamilyIndex = NextIndex;
		Pending->bComplete.store(true, std::memory_order_release);
	};

#if !UE_VERSION_OLDER_THAN(5, 4, 0)
	UE::Tasks::Launch(TEXT("Framedash::DirectSocketFallback"), MoveTemp(TaskBody),
		UE::Tasks::ETaskPriority::BackgroundNormal);
#else
	Async(EAsyncExecution::ThreadPool, MoveTemp(TaskBody));
#endif
}

#endif


bool FFramedashTransport::ValidateEndpointSecurity() const
{
	// A control character (including an embedded NUL) makes the URL malformed and
	// hostile: ToUtf8String converts via a NUL-terminated C string, so a NUL would
	// truncate the validated copy while the HTTP layer may still see the full
	// FString -- a parser differential. Reject such URLs outright, before convert.
	for (const auto& Ch : EndpointUrl)
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
