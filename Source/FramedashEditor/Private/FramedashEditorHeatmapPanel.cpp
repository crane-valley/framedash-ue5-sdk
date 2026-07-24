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

namespace
{
	TSharedPtr<int32> FindIntegerOption(const TArray<TSharedPtr<int32>>& Options, int32 Value)
	{
		for (const auto& Option : Options)
		{
			if (Option.IsValid() && *Option == Value)
			{
				return Option;
			}
		}
		return nullptr;
	}
}

void SFramedashEditorHeatmapPanel::Construct(const FArguments& Arguments)
{
	HttpClient = MakeShared<FFramedashEditorHttpClient>();
	Overlay = Arguments._Overlay;

	for (const auto& Value : {1, 7, 14, 30})
	{
		DaysOptions.Add(MakeShared<int32>(Value));
	}
	for (const auto& Value : {5, 10, 25, 50})
	{
		CellSizeOptions.Add(MakeShared<int32>(Value));
	}

	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	SelectedDays = FindIntegerOption(DaysOptions, Settings != nullptr ? Settings->Days : 7);
	SelectedCellSize = FindIntegerOption(CellSizeOptions, Settings != nullptr ? Settings->CellSize : 25);
	EventNameFilter = Settings != nullptr ? Settings->EventNameFilter : FString();
	ObservedReadApiKey = Settings != nullptr ? Settings->ReadApiKey : FString();
	ObservedApiBaseUrl = Settings != nullptr ? Settings->ApiBaseUrl : FString();
	ObservedProjectId = Settings != nullptr ? Settings->ProjectId : FString();
	ObservedEventNameFilter = EventNameFilter;
	ObservedDays = Settings != nullptr ? Settings->Days : 7;
	ObservedCellSize = Settings != nullptr ? Settings->CellSize : 25;
	SettingsChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this,
		&SFramedashEditorHeatmapPanel::OnSettingsObjectPropertyChanged);

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 12.0f, 12.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT(
					"Introduction",
					"Fetch cloud heatmap data outside PIE, then use Show > Framedash Heatmap in each level viewport."))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 0.0f, 12.0f, 10.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SettingsHint", "Configure the API URL, project ID, read key, opacity, alignment, and Z offset under Project Settings > Plugins > Framedash Heatmap."))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 0.0f)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 10.0f)
			[
				SNew(SGridPanel)
				.FillColumn(1, 1.0f)
				+ SGridPanel::Slot(0, 0)
				.Padding(0.0f, 4.0f, 12.0f, 4.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("MapLabel", "Map"))
				]
				+ SGridPanel::Slot(1, 0)
				.Padding(0.0f, 4.0f)
				[
					SNew(SBox)
					.MinDesiredWidth(FramedashEditor::HeatmapMapSelectorMinimumWidth)
					[
						SAssignNew(MapComboBox, SComboBox<TSharedPtr<FramedashEditor::FMapInfo>>)
						.OptionsSource(&MapOptions)
						.OnGenerateWidget(this, &SFramedashEditorHeatmapPanel::GenerateMapWidget)
						.OnSelectionChanged(this, &SFramedashEditorHeatmapPanel::OnMapSelected)
						.IsEnabled(this, &SFramedashEditorHeatmapPanel::CanStartRequest)
						[
							SNew(STextBlock).Text(this, &SFramedashEditorHeatmapPanel::GetSelectedMapText)
						]
					]
				]
				+ SGridPanel::Slot(1, 1)
				.Padding(0.0f, 4.0f)
				.HAlign(HAlign_Left)
				[
					SNew(SButton)
					.Text(LOCTEXT("RefreshMaps", "Refresh Maps"))
					.IsEnabled(this, &SFramedashEditorHeatmapPanel::CanStartRequest)
					.OnClicked(this, &SFramedashEditorHeatmapPanel::RefreshMaps)
				]
				+ SGridPanel::Slot(0, 2)
				.Padding(0.0f, 4.0f, 12.0f, 4.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("DaysLabel", "Days"))
				]
				+ SGridPanel::Slot(1, 2)
				.Padding(0.0f, 4.0f)
				[
					SAssignNew(DaysComboBox, SComboBox<TSharedPtr<int32>>)
					.OptionsSource(&DaysOptions)
					.InitiallySelectedItem(SelectedDays)
					.OnGenerateWidget(this, &SFramedashEditorHeatmapPanel::GenerateIntegerWidget)
					.OnSelectionChanged(this, &SFramedashEditorHeatmapPanel::OnDaysSelected)
					.IsEnabled(this, &SFramedashEditorHeatmapPanel::CanStartRequest)
					[
						SNew(STextBlock).Text(this, &SFramedashEditorHeatmapPanel::GetSelectedDaysText)
					]
				]
				+ SGridPanel::Slot(0, 3)
				.Padding(0.0f, 4.0f, 12.0f, 4.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("CellSizeLabel", "Cell Size"))
				]
				+ SGridPanel::Slot(1, 3)
				.Padding(0.0f, 4.0f)
				[
					SAssignNew(CellSizeComboBox, SComboBox<TSharedPtr<int32>>)
					.OptionsSource(&CellSizeOptions)
					.InitiallySelectedItem(SelectedCellSize)
					.OnGenerateWidget(this, &SFramedashEditorHeatmapPanel::GenerateIntegerWidget)
					.OnSelectionChanged(this, &SFramedashEditorHeatmapPanel::OnCellSizeSelected)
					.IsEnabled(this, &SFramedashEditorHeatmapPanel::CanStartRequest)
					[
						SNew(STextBlock).Text(this, &SFramedashEditorHeatmapPanel::GetSelectedCellSizeText)
					]
				]
				+ SGridPanel::Slot(0, 4)
				.Padding(0.0f, 4.0f, 12.0f, 4.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("EventNameLabel", "Event Name"))
				]
				+ SGridPanel::Slot(1, 4)
				.Padding(0.0f, 4.0f)
				[
					SNew(SEditableTextBox)
					.Text(this, &SFramedashEditorHeatmapPanel::GetEventNameFilterText)
					.HintText(LOCTEXT("EventNameHint", "Optional exact event name"))
					.IsEnabled(this, &SFramedashEditorHeatmapPanel::CanStartRequest)
					.OnTextChanged(this, &SFramedashEditorHeatmapPanel::OnEventNameChanged)
					.OnTextCommitted(this, &SFramedashEditorHeatmapPanel::OnEventNameCommitted)
				]
				+ SGridPanel::Slot(0, 5)
				.Padding(0.0f, 4.0f, 12.0f, 4.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("WorldAlignmentLabel", "World Alignment"))
					.ToolTipText(LOCTEXT(
						"WorldAlignmentTooltip",
						"Translate cloud coordinates into this editor world's origin without changing telemetry data."))
				]
				+ SGridPanel::Slot(1, 5)
				.Padding(0.0f, 4.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("WorldAlignmentXLabel", "X"))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(0.0f, 0.0f, 12.0f, 0.0f)
					[
						SAssignNew(WorldOffsetXEntry, SNumericEntryBox<double>)
						.AllowSpin(true)
						.MinSliderValue(-100000.0)
						.MaxSliderValue(100000.0)
						.Delta(10.0)
						.Value(this, &SFramedashEditorHeatmapPanel::GetWorldOffsetX)
						.OnValueChanged(this, &SFramedashEditorHeatmapPanel::OnWorldOffsetXChanged)
						.OnValueCommitted(this, &SFramedashEditorHeatmapPanel::OnWorldOffsetXCommitted)
						.OnEndSliderMovement(this, &SFramedashEditorHeatmapPanel::OnWorldOffsetXSliderEnded)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("WorldAlignmentYLabel", "Y"))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SAssignNew(WorldOffsetYEntry, SNumericEntryBox<double>)
						.AllowSpin(true)
						.MinSliderValue(-100000.0)
						.MaxSliderValue(100000.0)
						.Delta(10.0)
						.Value(this, &SFramedashEditorHeatmapPanel::GetWorldOffsetY)
						.OnValueChanged(this, &SFramedashEditorHeatmapPanel::OnWorldOffsetYChanged)
						.OnValueCommitted(this, &SFramedashEditorHeatmapPanel::OnWorldOffsetYCommitted)
						.OnEndSliderMovement(this, &SFramedashEditorHeatmapPanel::OnWorldOffsetYSliderEnded)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 0.0f, 12.0f, 10.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(SButton)
					.Text(this, &SFramedashEditorHeatmapPanel::GetFetchButtonText)
					.IsEnabled(this, &SFramedashEditorHeatmapPanel::CanFetchHeatmap)
					.OnClicked(this, &SFramedashEditorHeatmapPanel::FetchHeatmap)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("FrameHeatmap", "Frame Heatmap"))
					.IsEnabled(this, &SFramedashEditorHeatmapPanel::CanFrameHeatmap)
					.OnClicked(this, &SFramedashEditorHeatmapPanel::FrameHeatmap)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(12.0f, 0.0f, 12.0f, 12.0f)
			[
				SNew(SBox)
				.MinDesiredHeight(48.0f)
				[
					SNew(STextBlock)
					.Text(this, &SFramedashEditorHeatmapPanel::GetStatusText)
					.AutoWrapText(true)
				]
			]
		]
	];

	StartRefreshMaps(false);
}

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

TSharedRef<SWidget> SFramedashEditorHeatmapPanel::GenerateMapWidget(
	TSharedPtr<FramedashEditor::FMapInfo> Map) const
{
	return SNew(STextBlock)
		.Text(Map.IsValid() ? FText::FromString(Map->Name) : LOCTEXT("InvalidMap", "Invalid map"));
}

TSharedRef<SWidget> SFramedashEditorHeatmapPanel::GenerateIntegerWidget(TSharedPtr<int32> Value) const
{
	return SNew(STextBlock).Text(Value.IsValid() ? FText::AsNumber(*Value) : FText::GetEmpty());
}

FText SFramedashEditorHeatmapPanel::GetSelectedMapText() const
{
	return SelectedMap.IsValid() ? FText::FromString(SelectedMap->Name) : LOCTEXT("SelectMap", "Select a map");
}

FText SFramedashEditorHeatmapPanel::GetSelectedDaysText() const
{
	return SelectedDays.IsValid() ? FText::AsNumber(*SelectedDays) : FText::GetEmpty();
}

FText SFramedashEditorHeatmapPanel::GetSelectedCellSizeText() const
{
	return SelectedCellSize.IsValid() ? FText::AsNumber(*SelectedCellSize) : FText::GetEmpty();
}

FText SFramedashEditorHeatmapPanel::GetEventNameFilterText() const
{
	return FText::FromString(EventNameFilter);
}

TOptional<double> SFramedashEditorHeatmapPanel::GetWorldOffsetX() const
{
	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	return Settings != nullptr ? TOptional<double>(Settings->WorldOffsetX) : TOptional<double>();
}

TOptional<double> SFramedashEditorHeatmapPanel::GetWorldOffsetY() const
{
	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	return Settings != nullptr ? TOptional<double>(Settings->WorldOffsetY) : TOptional<double>();
}

FText SFramedashEditorHeatmapPanel::GetFetchButtonText() const
{
	return bBusy ? LOCTEXT("Busy", "Working...") : LOCTEXT("Fetch", "Fetch");
}

FText SFramedashEditorHeatmapPanel::GetStatusText() const
{
	return StatusText;
}

void SFramedashEditorHeatmapPanel::OnMapSelected(
	TSharedPtr<FramedashEditor::FMapInfo> Map,
	ESelectInfo::Type SelectionType)
{
	const bool bChanged = SelectedMap != Map;
	SelectedMap = MoveTemp(Map);
	if (bChanged && SelectionType != ESelectInfo::Direct && Overlay.IsValid())
	{
		Overlay->ClearData();
		StatusText = LOCTEXT("MapChanged", "Map changed. Fetch heatmap data for the new selection.");
	}
}

void SFramedashEditorHeatmapPanel::OnDaysSelected(
	TSharedPtr<int32> Value,
	ESelectInfo::Type SelectionType)
{
	if (!Value.IsValid())
	{
		return;
	}
	const bool bChanged = !SelectedDays.IsValid() || *SelectedDays != *Value;
	SelectedDays = Value;
	if (SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	if (UFramedashEditorSettings* Settings = GetMutableDefault<UFramedashEditorSettings>())
	{
		Settings->Days = *Value;
		ObservedDays = *Value;
		SaveSettings();
	}
	if (bChanged)
	{
		++QueryRevision;
		if (Overlay.IsValid())
		{
			Overlay->ClearData();
		}
		StatusText = LOCTEXT("DaysChanged", "Days changed. Fetch heatmap data for the new selection.");
	}
}

void SFramedashEditorHeatmapPanel::OnCellSizeSelected(
	TSharedPtr<int32> Value,
	ESelectInfo::Type SelectionType)
{
	if (!Value.IsValid())
	{
		return;
	}
	const bool bChanged = !SelectedCellSize.IsValid() || *SelectedCellSize != *Value;
	SelectedCellSize = Value;
	if (SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	if (UFramedashEditorSettings* Settings = GetMutableDefault<UFramedashEditorSettings>())
	{
		Settings->CellSize = *Value;
		ObservedCellSize = *Value;
		SaveSettings();
	}
	if (bChanged)
	{
		++QueryRevision;
		if (Overlay.IsValid())
		{
			Overlay->ClearData();
		}
		StatusText = LOCTEXT("CellSizeChanged", "Cell size changed. Fetch heatmap data for the new selection.");
	}
}

void SFramedashEditorHeatmapPanel::OnEventNameChanged(const FText& Text)
{
	EventNameFilter = Text.ToString();
}

void SFramedashEditorHeatmapPanel::OnEventNameCommitted(const FText& Text, ETextCommit::Type)
{
	const FString CommittedEventNameFilter = Text.ToString();
	const UFramedashEditorSettings* CurrentSettings = GetDefault<UFramedashEditorSettings>();
	const bool bChanged = CurrentSettings != nullptr
		? CurrentSettings->EventNameFilter != CommittedEventNameFilter
		: EventNameFilter != CommittedEventNameFilter;
	EventNameFilter = CommittedEventNameFilter;
	if (UFramedashEditorSettings* Settings = GetMutableDefault<UFramedashEditorSettings>())
	{
		Settings->EventNameFilter = EventNameFilter;
		ObservedEventNameFilter = EventNameFilter;
		SaveSettings();
	}
	if (bChanged)
	{
		++QueryRevision;
		if (Overlay.IsValid())
		{
			Overlay->ClearData();
		}
		StatusText = LOCTEXT("EventNameChanged", "Event name changed. Fetch heatmap data for the new selection.");
	}
}

void SFramedashEditorHeatmapPanel::OnWorldOffsetXChanged(double Value)
{
	if (UFramedashEditorSettings* Settings = GetMutableDefault<UFramedashEditorSettings>())
	{
		Settings->WorldOffsetX = Value;
	}
}

void SFramedashEditorHeatmapPanel::OnWorldOffsetYChanged(double Value)
{
	if (UFramedashEditorSettings* Settings = GetMutableDefault<UFramedashEditorSettings>())
	{
		Settings->WorldOffsetY = Value;
	}
}

void SFramedashEditorHeatmapPanel::OnWorldOffsetXCommitted(double Value, ETextCommit::Type)
{
	OnWorldOffsetXChanged(Value);
	ApplyWorldOffsetAndSave();
}

void SFramedashEditorHeatmapPanel::OnWorldOffsetYCommitted(double Value, ETextCommit::Type)
{
	OnWorldOffsetYChanged(Value);
	ApplyWorldOffsetAndSave();
}

void SFramedashEditorHeatmapPanel::OnWorldOffsetXSliderEnded(double Value)
{
	OnWorldOffsetXChanged(Value);
	ApplyWorldOffsetAndSave();
}

void SFramedashEditorHeatmapPanel::OnWorldOffsetYSliderEnded(double Value)
{
	OnWorldOffsetYChanged(Value);
	ApplyWorldOffsetAndSave();
}

void SFramedashEditorHeatmapPanel::ApplyWorldOffsetAndSave()
{
	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	if (Settings != nullptr && Overlay.IsValid())
	{
		Overlay->SetWorldOffset(FVector2D(Settings->WorldOffsetX, Settings->WorldOffsetY));
	}
	SaveSettings();
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
