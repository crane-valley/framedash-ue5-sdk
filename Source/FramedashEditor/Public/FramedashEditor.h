// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFramedashEditor, Log, All);

class SDockTab;
class FSpawnTabArgs;
class FFramedashEditorHeatmapOverlay;

class FFramedashEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

#if WITH_DEV_AUTOMATION_TESTS
	bool HasHeatmapOverlayForTesting() const
	{
		return HeatmapOverlay.IsValid();
	}

	TSharedPtr<FFramedashEditorHeatmapOverlay> GetHeatmapOverlayForTesting() const
	{
		return HeatmapOverlay;
	}
#endif

private:
	TSharedRef<SDockTab> SpawnHeatmapTab(const FSpawnTabArgs& SpawnTabArgs);
	void RegisterMenus();
	void OpenHeatmapTab();

	TSharedPtr<FFramedashEditorHeatmapOverlay> HeatmapOverlay;
};
