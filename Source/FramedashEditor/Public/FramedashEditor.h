// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFramedashEditor, Log, All);

class SDockTab;
class FSpawnTabArgs;

class FFramedashEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TSharedRef<SDockTab> SpawnHeatmapTab(const FSpawnTabArgs& SpawnTabArgs);
	void RegisterMenus();
	void OpenHeatmapTab();
};
