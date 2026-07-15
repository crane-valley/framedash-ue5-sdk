// Copyright Crane Valley. All Rights Reserved.

#include "FramedashEditor.h"

#include "FramedashEditorHeatmapPanel.h"

#include "Framework/Docking/TabManager.h"
#include "ToolMenu.h"
#include "ToolMenuEntry.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SNullWidget.h"

#define LOCTEXT_NAMESPACE "FFramedashEditorModule"

DEFINE_LOG_CATEGORY(LogFramedashEditor);

namespace
{
	const FName HeatmapTabName(TEXT("FramedashHeatmap"));
}

void FFramedashEditorModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		HeatmapTabName,
		FOnSpawnTab::CreateRaw(this, &FFramedashEditorModule::SpawnHeatmapTab))
		.SetDisplayName(LOCTEXT("HeatmapTabTitle", "Framedash Heatmap"))
		.SetTooltipText(LOCTEXT("HeatmapTabTooltip", "Fetch and display cloud-aggregated Framedash heatmaps."));

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FFramedashEditorModule::RegisterMenus));
	UE_LOG(LogFramedashEditor, Log, TEXT("Framedash editor module loaded"));
}

void FFramedashEditorModule::ShutdownModule()
{
	if (const TSharedPtr<SDockTab> LiveTab = FGlobalTabmanager::Get()->FindExistingLiveTab(HeatmapTabName))
	{
		// Destroy request and draw delegates before this module's code is unloaded.
		LiveTab->SetContent(SNullWidget::NullWidget);
		LiveTab->RequestCloseTab();
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(HeatmapTabName);
	UE_LOG(LogFramedashEditor, Log, TEXT("Framedash editor module unloaded"));
}

TSharedRef<SDockTab> FFramedashEditorModule::SpawnHeatmapTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SFramedashEditorHeatmapPanel)
		];
}

void FFramedashEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
	if (WindowMenu == nullptr)
	{
		return;
	}
	FToolMenuSection& Section = WindowMenu->FindOrAddSection(TEXT("Framedash"));
	Section.AddMenuEntry(
		TEXT("OpenFramedashHeatmap"),
		LOCTEXT("OpenHeatmapLabel", "Framedash Heatmap"),
		LOCTEXT("OpenHeatmapTooltip", "Open the Framedash cloud heatmap overlay."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FFramedashEditorModule::OpenHeatmapTab)));
}

void FFramedashEditorModule::OpenHeatmapTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(HeatmapTabName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFramedashEditorModule, FramedashEditor)
