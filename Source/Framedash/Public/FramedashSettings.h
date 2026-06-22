// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FramedashSettings.generated.h"

/**
 * Framedash SDK settings, accessible via Project Settings > Plugins > Framedash.
 */
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Framedash"))
class FRAMEDASH_API UFramedashSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFramedashSettings();

	/** API key for authenticating with the Framedash ingest endpoint. */
	UPROPERTY(Config, EditAnywhere, Category="Authentication", meta=(DisplayName="API Key"))
	FString ApiKey;

	/** Ingest endpoint URL. */
	UPROPERTY(Config, EditAnywhere, Category="Connection", meta=(DisplayName="Endpoint URL"))
	FString EndpointUrl;

	/** Build identifier included with every event. */
	UPROPERTY(Config, EditAnywhere, Category="Metadata", meta=(DisplayName="Build ID"))
	FString BuildId;

	/** Developer-supplied player identifier. Leave empty for anonymous telemetry. */
	UPROPERTY(Config, EditAnywhere, Category="Metadata", meta=(DisplayName="Player ID"))
	FString PlayerId;

	/** Probability of recording each event (0.0 = none, 1.0 = all). */
	UPROPERTY(Config, EditAnywhere, Category="Sampling", meta=(ClampMin="0.0", ClampMax="1.0", DisplayName="Sampling Rate"))
	float SamplingRate;

	/** If true, the subsystem auto-initializes on GameInstance startup. */
	UPROPERTY(Config, EditAnywhere, Category="General", meta=(DisplayName="Auto Initialize"))
	bool bAutoInitialize;

	/** If true, unsent events are saved under Project/Saved/Framedash and retried next run. */
	UPROPERTY(Config, EditAnywhere, Category="Persistence", meta=(DisplayName="Enable Offline Queue"))
	bool bEnableOfflineQueue;

	/** If true, the SDK records the local player's camera yaw/pitch on each event (for direction heatmaps). */
	UPROPERTY(Config, EditAnywhere, Category="Capture", meta=(DisplayName="Capture Camera Rotation"))
	bool bCaptureCameraRotation;

	/** Category path in Project Settings. */
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
};
