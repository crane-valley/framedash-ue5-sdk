// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FramedashEditorSettings.generated.h"

UCLASS(Config=EditorPerProjectUserSettings, meta=(DisplayName="Framedash Heatmap"))
class FRAMEDASHEDITOR_API UFramedashEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFramedashEditorSettings();

	/** Read-only API key carrying the analytics:read scope. */
	UPROPERTY(Config, EditAnywhere, Category="Authentication", meta=(DisplayName="Read API Key"))
	FString ReadApiKey;

	/** Framedash application base URL used by the editor REST client. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="API Base URL"))
	FString ApiBaseUrl;

	/** Project UUID whose maps and aggregate heatmaps are queried. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Project ID"))
	FString ProjectId;

	/** Aggregate lookback window. Supported values are 1, 7, 14, and 30. */
	UPROPERTY(Config, EditAnywhere, Category="Query", meta=(DisplayName="Days"))
	int32 Days;

	/** Heatmap cell size in UE world units. Supported values are 5, 10, 25, and 50. */
	UPROPERTY(Config, EditAnywhere, Category="Query", meta=(DisplayName="Cell Size"))
	int32 CellSize;

	/** Optional exact event-name filter. */
	UPROPERTY(Config, EditAnywhere, Category="Query", meta=(DisplayName="Event Name Filter"))
	FString EventNameFilter;

	/** Opacity applied to every heatmap cell. */
	UPROPERTY(Config, EditAnywhere, Category="Overlay", meta=(ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0"))
	float OverlayOpacity;

	/** Editor-only X translation used when telemetry and level origins differ. */
	UPROPERTY(Config, EditAnywhere, Category="Overlay", meta=(DisplayName="World Alignment X"))
	double WorldOffsetX;

	/** Editor-only Y translation used when telemetry and level origins differ. */
	UPROPERTY(Config, EditAnywhere, Category="Overlay", meta=(DisplayName="World Alignment Y"))
	double WorldOffsetY;

	/** Editor-only vertical translation applied to measured voxel positions. */
	UPROPERTY(Config, EditAnywhere, Category="Overlay", meta=(DisplayName="Z Offset"))
	float ZOffset;

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
	virtual FName GetSectionName() const override { return FName(TEXT("FramedashHeatmap")); }
	virtual FText GetSectionText() const override;
};
