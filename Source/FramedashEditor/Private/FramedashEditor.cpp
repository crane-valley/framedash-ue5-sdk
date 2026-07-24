// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditor.h"

#include "FramedashEditorHeatmapOverlay.h"
#include "FramedashEditorHeatmapPanel.h"

#include "Brushes/SlateImageBrush.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
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
	const FName EditorStyleName(TEXT("FramedashEditorStyle"));
	const FName HeatmapIconName(TEXT("FramedashEditor.Heatmap"));
	TSharedPtr<FSlateStyleSet> EditorStyle;

	void RegisterEditorStyle()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Framedash"));
		if (!Plugin.IsValid())
		{
			return;
		}

		EditorStyle = MakeShared<FSlateStyleSet>(EditorStyleName);
		EditorStyle->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
		EditorStyle->Set(
			HeatmapIconName,
			new FSlateImageBrush(
				EditorStyle->RootToContentDir(TEXT("Icon128"), TEXT(".png")),
				FVector2D(16.0f, 16.0f)));
		FSlateStyleRegistry::RegisterSlateStyle(*EditorStyle);
	}

	void UnregisterEditorStyle()
	{
		if (EditorStyle.IsValid())
		{
			FSlateStyleRegistry::UnRegisterSlateStyle(*EditorStyle);
			EditorStyle.Reset();
		}
	}
}

void FFramedashEditorModule::StartupModule()
{
	RegisterEditorStyle();
	HeatmapOverlay = MakeShared<FFramedashEditorHeatmapOverlay>();
	const FSlateIcon HeatmapIcon(EditorStyleName, HeatmapIconName);
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		HeatmapTabName,
		FOnSpawnTab::CreateRaw(this, &FFramedashEditorModule::SpawnHeatmapTab))
		.SetDisplayName(LOCTEXT("HeatmapTabTitle", "Framedash Heatmap"))
		.SetIcon(HeatmapIcon)
		.SetMenuType(ETabSpawnerMenuType::Hidden)
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
	if (HeatmapOverlay.IsValid())
	{
		HeatmapOverlay->Shutdown();
		HeatmapOverlay.Reset();
	}
	UnregisterEditorStyle();
	UE_LOG(LogFramedashEditor, Log, TEXT("Framedash editor module unloaded"));
}

TSharedRef<SDockTab> FFramedashEditorModule::SpawnHeatmapTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SFramedashEditorHeatmapPanel)
			.Overlay(HeatmapOverlay)
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
	UToolMenu* FramedashMenu = WindowMenu->AddSubMenu(
		FToolMenuOwner(this),
		TEXT("GetContent"),
		TEXT("Framedash"),
		LOCTEXT("FramedashMenuLabel", "Framedash"),
		LOCTEXT("FramedashMenuTooltip", "Open Framedash editor tools."));
	if (FramedashMenu == nullptr)
	{
		return;
	}
	if (FToolMenuSection* ParentSection = WindowMenu->FindSection(TEXT("GetContent")))
	{
		if (FToolMenuEntry* ParentEntry = ParentSection->FindEntry(TEXT("Framedash")))
		{
			ParentEntry->Icon = FSlateIcon(EditorStyleName, HeatmapIconName);
		}
	}

	FToolMenuSection& Section = FramedashMenu->FindOrAddSection(NAME_None);
	Section.AddMenuEntry(
		TEXT("OpenFramedashHeatmap"),
		LOCTEXT("OpenHeatmapLabel", "Framedash Heatmap"),
		LOCTEXT("OpenHeatmapTooltip", "Open the Framedash cloud heatmap overlay."),
		FSlateIcon(EditorStyleName, HeatmapIconName),
		FUIAction(FExecuteAction::CreateRaw(this, &FFramedashEditorModule::OpenHeatmapTab)));
}

void FFramedashEditorModule::OpenHeatmapTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(HeatmapTabName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFramedashEditorModule, FramedashEditor)
