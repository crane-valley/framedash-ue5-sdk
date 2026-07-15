// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"

#include "FramedashEditorLogic.h"

class UFramedashEditorSettings;

class FFramedashEditorHttpClient : public TSharedFromThis<FFramedashEditorHttpClient>
{
public:
	using FMapsCallback = TFunction<void(bool, TArray<FramedashEditor::FMapInfo>&&, const FString&)>;
	using FHeatmapCallback = TFunction<void(bool, TArray<FramedashEditor::FHeatmapCell>&&, const FString&)>;

	void FetchMaps(const UFramedashEditorSettings& Settings, FMapsCallback Callback);
	void FetchHeatmap(
		const UFramedashEditorSettings& Settings,
		const FString& MapId,
		FHeatmapCallback Callback);
	void Shutdown();

	~FFramedashEditorHttpClient();

private:
	using FResponseCallback = TFunction<void(bool, int32, FString&&, const FString&)>;

	bool PrepareRequest(
		const UFramedashEditorSettings& Settings,
		FString& OutBaseUrl,
		FString& OutApiKey,
		FString& OutProjectId,
		FString& OutError) const;
	void StartGet(const FString& Url, const FString& ApiKey, FResponseCallback Callback);
	void RemoveRequest(const FHttpRequestPtr& Request);

	TArray<FHttpRequestPtr> ActiveRequests;
	bool bShuttingDown = false;
};
