// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorHttpClient.h"

#include "FramedashEditorEndpointSecurity.h"
#include "FramedashEditorSettings.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/EngineVersionComparison.h"

namespace
{
	bool IsAllowedDays(int32 Days)
	{
		return Days == 1 || Days == 7 || Days == 14 || Days == 30;
	}

	bool IsAllowedCellSize(int32 CellSize)
	{
		return CellSize == 5 || CellSize == 10 || CellSize == 25 || CellSize == 50;
	}

	FString HttpFallback(int32 StatusCode)
	{
		return StatusCode > 0
			? FString::Printf(TEXT("Framedash request failed (HTTP %d)."), StatusCode)
			: FString(TEXT("Framedash request failed."));
	}
}

FFramedashEditorHttpClient::~FFramedashEditorHttpClient()
{
	Shutdown();
}

void FFramedashEditorHttpClient::FetchMaps(
	const UFramedashEditorSettings& Settings,
	FMapsCallback Callback)
{
	FString BaseUrl;
	FString ApiKey;
	FString ProjectId;
	FString Error;
	if (!PrepareRequest(Settings, BaseUrl, ApiKey, ProjectId, Error))
	{
		Callback(false, {}, Error);
		return;
	}

	const FString Url = FString::Printf(
		TEXT("%s/api/v1/projects/%s/maps"),
		*BaseUrl,
		*FGenericPlatformHttp::UrlEncode(ProjectId));
	StartGet(Url, ApiKey,
		[Callback = MoveTemp(Callback)](
			bool bConnected,
			int32 StatusCode,
			FString&& Body,
			const FString& RequestError) mutable
		{
			if (!bConnected)
			{
				Callback(false, {}, RequestError.IsEmpty()
					? FString(TEXT("Unable to reach the Framedash API."))
					: RequestError);
				return;
			}
			if (StatusCode != 200)
			{
				Callback(false, {}, FramedashEditor::ParseProblemMessage(Body, HttpFallback(StatusCode)));
				return;
			}

			TArray<FramedashEditor::FMapInfo> Maps;
			FString ParseError;
			if (!FramedashEditor::ParseMapsResponse(Body, Maps, ParseError))
			{
				Callback(false, {}, ParseError);
				return;
			}
			Callback(true, MoveTemp(Maps), FString());
		});
}

void FFramedashEditorHttpClient::FetchHeatmap(
	const UFramedashEditorSettings& Settings,
	const FString& MapId,
	FHeatmapCallback Callback)
{
	FString BaseUrl;
	FString ApiKey;
	FString ProjectId;
	FString Error;
	if (!PrepareRequest(Settings, BaseUrl, ApiKey, ProjectId, Error))
	{
		Callback(false, {}, Error);
		return;
	}
	if (MapId.IsEmpty())
	{
		Callback(false, {}, TEXT("Select a map before fetching heatmap data."));
		return;
	}
	if (!IsAllowedDays(Settings.Days))
	{
		Callback(false, {}, TEXT("Days must be one of 1, 7, 14, or 30."));
		return;
	}
	if (!IsAllowedCellSize(Settings.CellSize))
	{
		Callback(false, {}, TEXT("Cell size must be one of 5, 10, 25, or 50."));
		return;
	}

	FString Url = FString::Printf(
		TEXT("%s/api/v1/projects/%s/heatmap?mapId=%s&cellSize=%d&days=%d"),
		*BaseUrl,
		*FGenericPlatformHttp::UrlEncode(ProjectId),
		*FGenericPlatformHttp::UrlEncode(MapId),
		Settings.CellSize,
		Settings.Days);
	if (!Settings.EventNameFilter.IsEmpty())
	{
		Url += TEXT("&eventName=") + FGenericPlatformHttp::UrlEncode(Settings.EventNameFilter);
	}

	StartGet(Url, ApiKey,
		[Callback = MoveTemp(Callback)](
			bool bConnected,
			int32 StatusCode,
			FString&& Body,
			const FString& RequestError) mutable
		{
			if (!bConnected)
			{
				Callback(false, {}, RequestError.IsEmpty()
					? FString(TEXT("Unable to reach the Framedash API."))
					: RequestError);
				return;
			}
			if (StatusCode != 200)
			{
				Callback(false, {}, FramedashEditor::ParseProblemMessage(Body, HttpFallback(StatusCode)));
				return;
			}

			TArray<FramedashEditor::FHeatmapCell> Cells;
			FString ParseError;
			if (!FramedashEditor::ParseHeatmapResponse(Body, Cells, ParseError))
			{
				Callback(false, {}, ParseError);
				return;
			}
			Callback(true, MoveTemp(Cells), FString());
		});
}

void FFramedashEditorHttpClient::Shutdown()
{
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	for (const FHttpRequestPtr& Request : ActiveRequests)
	{
		if (Request.IsValid())
		{
			Request->OnProcessRequestComplete().Unbind();
			Request->CancelRequest();
		}
	}
	ActiveRequests.Reset();
}

bool FFramedashEditorHttpClient::PrepareRequest(
	const UFramedashEditorSettings& Settings,
	FString& OutBaseUrl,
	FString& OutApiKey,
	FString& OutProjectId,
	FString& OutError) const
{
	if (bShuttingDown)
	{
		OutError = TEXT("Framedash editor client is shutting down.");
		return false;
	}

	OutBaseUrl = Settings.ApiBaseUrl.TrimStartAndEnd();
	while (OutBaseUrl.RemoveFromEnd(TEXT("/")))
	{
	}
	OutApiKey = Settings.ReadApiKey.TrimStartAndEnd();
	OutProjectId = Settings.ProjectId.TrimStartAndEnd();
	if (OutBaseUrl.IsEmpty())
	{
		OutError = TEXT("Configure the Framedash API base URL in Project Settings.");
		return false;
	}

	const FTCHARToUTF8 Utf8Url(*OutBaseUrl);
	if (!FramedashEditor::IsEndpointSecure(
			std::string_view(Utf8Url.Get(), static_cast<std::size_t>(Utf8Url.Length()))))
	{
		OutError = TEXT("The API base URL is not secure. Use HTTPS, or HTTP only for canonical localhost.");
		return false;
	}
	if (OutApiKey.IsEmpty())
	{
		OutError = TEXT("Configure an analytics:read API key in Project Settings.");
		return false;
	}
	if (OutProjectId.IsEmpty())
	{
		OutError = TEXT("Configure a Framedash project ID in Project Settings.");
		return false;
	}
	return true;
}

void FFramedashEditorHttpClient::StartGet(
	const FString& Url,
	const FString& ApiKey,
	FResponseCallback Callback)
{
	if (bShuttingDown)
	{
		Callback(false, 0, FString(), FString());
		return;
	}

	const FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json, application/problem+json"));
	Request->SetHeader(TEXT("X-API-Key"), ApiKey);

	const TWeakPtr<FFramedashEditorHttpClient> WeakSelf = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[WeakSelf, Callback, Url](
			FHttpRequestPtr CompletedRequest,
			FHttpResponsePtr Response,
			bool bConnectedSuccessfully) mutable
		{
			const TSharedPtr<FFramedashEditorHttpClient> Self = WeakSelf.Pin();
			if (!Self.IsValid() || Self->bShuttingDown)
			{
				return;
			}
			Self->RemoveRequest(CompletedRequest);

#if !UE_VERSION_OLDER_THAN(5, 4, 0)
			const FString EffectiveUrl = CompletedRequest.IsValid()
				? CompletedRequest->GetEffectiveURL()
				: FString();
			if (!EffectiveUrl.IsEmpty() && Url != EffectiveUrl)
			{
				const FTCHARToUTF8 ConfiguredUtf8(*Url);
				const FTCHARToUTF8 EffectiveUtf8(*EffectiveUrl);
				if (FramedashEditor::IsCrossOriginRedirect(
						std::string_view(ConfiguredUtf8.Get(), static_cast<std::size_t>(ConfiguredUtf8.Length())),
						std::string_view(EffectiveUtf8.Get(), static_cast<std::size_t>(EffectiveUtf8.Length()))))
				{
					Callback(false, 0, FString(),
						TEXT("Framedash request was redirected across origins; the response was rejected because the analytics read key may have been exposed."));
					return;
				}
			}
#endif

			const int32 StatusCode = Response.IsValid() ? Response->GetResponseCode() : 0;
			FString Body = Response.IsValid() ? Response->GetContentAsString() : FString();
			Callback(bConnectedSuccessfully && Response.IsValid(), StatusCode, MoveTemp(Body), FString());
		});

	ActiveRequests.Add(Request);
	if (!Request->ProcessRequest())
	{
		Request->OnProcessRequestComplete().Unbind();
		RemoveRequest(Request);
		Callback(false, 0, FString(), FString());
	}
}

void FFramedashEditorHttpClient::RemoveRequest(const FHttpRequestPtr& Request)
{
	ActiveRequests.Remove(Request);
}
