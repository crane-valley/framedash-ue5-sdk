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

TSharedPtr<int32> SFramedashEditorHeatmapPanel::FindIntegerOption(const TArray<TSharedPtr<int32>>& Options, int32 Value)
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

#undef LOCTEXT_NAMESPACE
