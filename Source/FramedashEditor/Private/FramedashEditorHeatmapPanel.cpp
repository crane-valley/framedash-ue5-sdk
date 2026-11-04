// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorHeatmapPanel.h"

#include "FramedashEditorHeatmapOverlay.h"
#include "FramedashEditorHeatmapLayout.h"
#include "FramedashEditorHttpClient.h"
#include "FramedashEditorSettings.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Editor.h"
#include "LevelEditorViewport.h"

#define LOCTEXT_NAMESPACE "FramedashEditorHeatmapPanel"

SFramedashEditorHeatmapPanel::~SFramedashEditorHeatmapPanel()
{
	if (SettingsChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(SettingsChangedHandle);
		SettingsChangedHandle.Reset();
	}
	if (HttpClient.IsValid())
	{
		HttpClient->Shutdown();
	}
}

FReply SFramedashEditorHeatmapPanel::RefreshMaps()
{
	StartRefreshMaps(true);
	return FReply::Handled();
}

void SFramedashEditorHeatmapPanel::StartRefreshMaps(bool bClearOverlay)
{
	if (!CanStartRequest() || !HttpClient.IsValid())
	{
		return;
	}
	bBusy = true;
	StatusText = LOCTEXT("LoadingMaps", "Loading maps...");
	SelectedMap.Reset();
	MapOptions.Reset();
	if (MapComboBox.IsValid())
	{
		MapComboBox->RefreshOptions();
	}
	if (bClearOverlay && Overlay.IsValid())
	{
		Overlay->ClearData();
	}

	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	if (Settings == nullptr)
	{
		bBusy = false;
		StatusText = LOCTEXT("SettingsUnavailable", "Framedash editor settings are unavailable.");
		return;
	}
	const uint64 RequestQueryRevision = QueryRevision;
	const TWeakPtr<SFramedashEditorHeatmapPanel> WeakSelf = SharedThis(this);
	HttpClient->FetchMaps(*Settings,
		[WeakSelf, RequestQueryRevision](
			bool bSuccess,
			TArray<FramedashEditor::FMapInfo>&& Maps,
			const FString& Error)
		{
			const TSharedPtr<SFramedashEditorHeatmapPanel> PinnedSelf = WeakSelf.Pin();
			if (!PinnedSelf.IsValid())
			{
				return;
			}
			PinnedSelf->HandleMapsResponse(
				bSuccess,
				RequestQueryRevision,
				MoveTemp(Maps),
				Error);
		});
}

FReply SFramedashEditorHeatmapPanel::FetchHeatmap()
{
	if (!CanFetchHeatmap() || !HttpClient.IsValid())
	{
		return FReply::Handled();
	}
	if (!SelectedDays.IsValid() || !SelectedCellSize.IsValid())
	{
		StatusText = LOCTEXT(
			"InvalidQuerySettings",
			"Days/Cell Size in Project Settings is not one of the allowed values (1/7/14/30 or 5/10/25/50).");
		return FReply::Handled();
	}

	UFramedashEditorSettings* Settings = GetMutableDefault<UFramedashEditorSettings>();
	if (Settings == nullptr)
	{
		StatusText = LOCTEXT("SettingsUnavailable", "Framedash editor settings are unavailable.");
		return FReply::Handled();
	}
	Settings->EventNameFilter = EventNameFilter;
	ObservedEventNameFilter = EventNameFilter;
	SaveSettings();

	bBusy = true;
	StatusText = LOCTEXT("LoadingHeatmap", "Fetching cloud heatmap cells...");
	if (Overlay.IsValid())
	{
		Overlay->ClearData();
	}
	const FramedashEditor::FMapInfo Map = *SelectedMap;
	const double CellSize = static_cast<double>(Settings->CellSize);
	const uint64 RequestQueryRevision = QueryRevision;
	const uint64 RequestOverlayRevision = Overlay.IsValid()
		? Overlay->GetDataRevision()
		: 0;
	const TWeakPtr<SFramedashEditorHeatmapPanel> WeakSelf = SharedThis(this);
	// The route resolves the slug (mapId) before falling back to the row UUID (id),
	// so a UUID that happens to collide with another map's user-supplied slug would
	// mis-resolve to the wrong map if the row id were sent here instead.
	HttpClient->FetchHeatmap(*Settings, Map.MapId,
		[WeakSelf, Map, CellSize, RequestQueryRevision, RequestOverlayRevision](
			bool bSuccess,
			TArray<FramedashEditor::FHeatmapCell>&& Cells,
			const FString& Error)
		{
			const TSharedPtr<SFramedashEditorHeatmapPanel> PinnedSelf = WeakSelf.Pin();
			if (!PinnedSelf.IsValid())
			{
				return;
			}
			PinnedSelf->HandleHeatmapResponse(
				bSuccess,
				Map,
				CellSize,
				RequestQueryRevision,
				RequestOverlayRevision,
				MoveTemp(Cells),
				Error);
		});
	return FReply::Handled();
}

bool SFramedashEditorHeatmapPanel::CanFetchHeatmap() const
{
	return CanStartRequest() && SelectedMap.IsValid();
}

FReply SFramedashEditorHeatmapPanel::FrameHeatmap()
{
	FBox Bounds;
	if (!Overlay.IsValid() || !Overlay->GetWorldBounds(Bounds) ||
		GCurrentLevelEditingViewportClient == nullptr)
	{
		StatusText = LOCTEXT(
			"FrameHeatmapUnavailable",
			"Fetch heatmap data before framing the viewport.");
		return FReply::Handled();
	}

	GCurrentLevelEditingViewportClient->SetViewportType(LVT_Perspective);
	GCurrentLevelEditingViewportClient->SetViewRotation(FRotator(-55.0, -45.0, 0.0));
	GCurrentLevelEditingViewportClient->FocusViewportOnBox(Bounds, true);
	GCurrentLevelEditingViewportClient->Invalidate();
	return FReply::Handled();
}

bool SFramedashEditorHeatmapPanel::CanFrameHeatmap() const
{
	FBox Bounds;
	return Overlay.IsValid() && Overlay->GetWorldBounds(Bounds) &&
		GCurrentLevelEditingViewportClient != nullptr &&
		(GEditor == nullptr || GEditor->PlayWorld == nullptr);
}

bool SFramedashEditorHeatmapPanel::CanStartRequest() const
{
	return !bBusy &&
		(GEditor == nullptr || GEditor->PlayWorld == nullptr);
}

void SFramedashEditorHeatmapPanel::OnSettingsObjectPropertyChanged(
	UObject* Object,
	FPropertyChangedEvent& PropertyChangedEvent)
{
	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	if (Settings == nullptr || Object != Settings)
	{
		return;
	}

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const bool bCheckAllQueryProperties = PropertyName.IsNone();
	const bool bDaysProperty = bCheckAllQueryProperties ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, Days);
	const bool bCellSizeProperty = bCheckAllQueryProperties ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, CellSize);
	const bool bEventNameProperty = bCheckAllQueryProperties ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, EventNameFilter);
	const bool bReadApiKeyProperty = bCheckAllQueryProperties ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, ReadApiKey);
	const bool bApiBaseUrlProperty = bCheckAllQueryProperties ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, ApiBaseUrl);
	const bool bProjectIdProperty = bCheckAllQueryProperties ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, ProjectId);

	if (!bDaysProperty && !bCellSizeProperty && !bEventNameProperty &&
		!bReadApiKeyProperty && !bApiBaseUrlProperty && !bProjectIdProperty)
	{
		return;
	}

	const bool bDaysChanged = bDaysProperty && ObservedDays != Settings->Days;
	const bool bCellSizeChanged =
		bCellSizeProperty && ObservedCellSize != Settings->CellSize;
	const bool bEventNameChanged =
		bEventNameProperty && ObservedEventNameFilter != Settings->EventNameFilter;
	const bool bReadApiKeyChanged =
		bReadApiKeyProperty && ObservedReadApiKey != Settings->ReadApiKey;
	const bool bApiBaseUrlChanged =
		bApiBaseUrlProperty && ObservedApiBaseUrl != Settings->ApiBaseUrl;
	const bool bProjectIdChanged =
		bProjectIdProperty && ObservedProjectId != Settings->ProjectId;
	const bool bConnectionChanged =
		bReadApiKeyChanged || bApiBaseUrlChanged || bProjectIdChanged;
	if (!bDaysChanged && !bCellSizeChanged && !bEventNameChanged && !bConnectionChanged)
	{
		return;
	}

	if (bDaysChanged)
	{
		ObservedDays = Settings->Days;
		SelectedDays = FindIntegerOption(DaysOptions, Settings->Days);
		if (DaysComboBox.IsValid())
		{
			if (SelectedDays.IsValid())
			{
				DaysComboBox->SetSelectedItem(SelectedDays);
			}
			else
			{
				DaysComboBox->ClearSelection();
			}
		}
	}
	if (bCellSizeChanged)
	{
		ObservedCellSize = Settings->CellSize;
		SelectedCellSize = FindIntegerOption(CellSizeOptions, Settings->CellSize);
		if (CellSizeComboBox.IsValid())
		{
			if (SelectedCellSize.IsValid())
			{
				CellSizeComboBox->SetSelectedItem(SelectedCellSize);
			}
			else
			{
				CellSizeComboBox->ClearSelection();
			}
		}
	}
	if (bEventNameChanged)
	{
		ObservedEventNameFilter = Settings->EventNameFilter;
		EventNameFilter = Settings->EventNameFilter;
	}
	if (bConnectionChanged)
	{
		ObservedReadApiKey = Settings->ReadApiKey;
		ObservedApiBaseUrl = Settings->ApiBaseUrl;
		ObservedProjectId = Settings->ProjectId;
		SelectedMap.Reset();
		MapOptions.Reset();
		if (MapComboBox.IsValid())
		{
			MapComboBox->ClearSelection();
			MapComboBox->RefreshOptions();
		}
	}
	++QueryRevision;
	if (Overlay.IsValid())
	{
		Overlay->ClearData();
	}
	StatusText = bConnectionChanged
		? LOCTEXT(
			"ConnectionSettingsChangedExternally",
			"Connection settings changed. Refresh maps for the new project.")
		: LOCTEXT(
			"QuerySettingsChangedExternally",
			"Query settings changed in Project Settings. Fetch heatmap data for the new selection.");
}

void SFramedashEditorHeatmapPanel::HandleMapsResponse(
	bool bSuccess,
	uint64 RequestQueryRevision,
	TArray<FramedashEditor::FMapInfo>&& Maps,
	const FString& Error)
{
	bBusy = false;
	if (RequestQueryRevision != QueryRevision)
	{
		StatusText = LOCTEXT(
			"MapsResponseStale",
			"Settings changed while fetching maps. Refresh maps for the current project.");
		return;
	}
	if (!bSuccess)
	{
		StatusText = FText::FromString(Error);
		return;
	}

	MapOptions.Reserve(Maps.Num());
	for (auto&& Map : Maps)
	{
		MapOptions.Add(MakeShared<FramedashEditor::FMapInfo>(MoveTemp(Map)));
	}
	if (MapComboBox.IsValid())
	{
		MapComboBox->RefreshOptions();
	}
	if (MapOptions.IsEmpty())
	{
		StatusText = LOCTEXT("NoMaps", "No maps were returned for this project.");
		return;
	}

	SelectedMap = MapOptions[0];
	if (MapComboBox.IsValid())
	{
		MapComboBox->SetSelectedItem(SelectedMap);
	}
	StatusText = FText::Format(
		LOCTEXT("MapsLoaded", "Loaded {0} map(s). Select a map and fetch its heatmap."),
		FText::AsNumber(MapOptions.Num()));
}

void SFramedashEditorHeatmapPanel::HandleHeatmapResponse(
	bool bSuccess,
	const FramedashEditor::FMapInfo& Map,
	double CellSize,
	uint64 RequestQueryRevision,
	uint64 RequestOverlayRevision,
	TArray<FramedashEditor::FHeatmapCell>&& Cells,
	const FString& Error)
{
	bBusy = false;
	if (RequestQueryRevision != QueryRevision ||
		!Overlay.IsValid() ||
		!Overlay->IsDataRevisionCurrent(RequestOverlayRevision))
	{
		StatusText = LOCTEXT(
			"HeatmapResponseStale",
			"The level or query settings changed while fetching. Fetch heatmap data again.");
		return;
	}
	if (!bSuccess)
	{
		StatusText = FText::FromString(Error);
		return;
	}

	const int32 CellCount = Cells.Num();
	if (Overlay.IsValid())
	{
		const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
		const FVector2D WorldOffset = Settings != nullptr
			? FVector2D(Settings->WorldOffsetX, Settings->WorldOffsetY)
			: FVector2D::ZeroVector;
		Overlay->SetData(Map, Cells, CellSize, WorldOffset);
	}
	if (CellCount == 10000)
	{
		StatusText = FText::Format(
			LOCTEXT(
				"CellsLoadedTruncated",
				"Loaded {0} cells. Results may be truncated at the API limit of 10,000 cells. Use Show > Framedash Heatmap in a level viewport."),
			FText::AsNumber(CellCount));
	}
	else
	{
		StatusText = FText::Format(
			LOCTEXT(
				"CellsLoaded",
				"Loaded {0} heatmap cell(s). Use Show > Framedash Heatmap in a level viewport."),
			FText::AsNumber(CellCount));
	}
}

void SFramedashEditorHeatmapPanel::SaveSettings()
{
	if (UFramedashEditorSettings* Settings = GetMutableDefault<UFramedashEditorSettings>())
	{
		Settings->SaveConfig();
	}
}

#undef LOCTEXT_NAMESPACE
