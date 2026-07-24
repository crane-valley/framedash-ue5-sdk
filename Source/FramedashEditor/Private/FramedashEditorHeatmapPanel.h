// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

#include "FramedashEditorLogic.h"

class FFramedashEditorHeatmapOverlay;
class FFramedashEditorHttpClient;
class UObject;
struct FPropertyChangedEvent;
template<typename OptionType> class SComboBox;
template<typename NumericType> class SNumericEntryBox;

class SFramedashEditorHeatmapPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFramedashEditorHeatmapPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FFramedashEditorHeatmapOverlay>, Overlay)
	SLATE_END_ARGS()

	void Construct(const FArguments& Arguments);
	virtual ~SFramedashEditorHeatmapPanel() override;

private:
	FReply RefreshMaps();
	void StartRefreshMaps(bool bClearOverlay);
	FReply FetchHeatmap();
	FReply FrameHeatmap();
	bool CanFetchHeatmap() const;
	bool CanFrameHeatmap() const;
	bool CanStartRequest() const;

	TSharedRef<SWidget> GenerateMapWidget(TSharedPtr<FramedashEditor::FMapInfo> Map) const;
	TSharedRef<SWidget> GenerateIntegerWidget(TSharedPtr<int32> Value) const;
	FText GetSelectedMapText() const;
	FText GetSelectedDaysText() const;
	FText GetSelectedCellSizeText() const;
	FText GetEventNameFilterText() const;
	TOptional<double> GetWorldOffsetX() const;
	TOptional<double> GetWorldOffsetY() const;
	FText GetFetchButtonText() const;
	FText GetStatusText() const;

	void OnMapSelected(
		TSharedPtr<FramedashEditor::FMapInfo> Map,
		ESelectInfo::Type SelectionType);
	void OnDaysSelected(TSharedPtr<int32> Value, ESelectInfo::Type SelectionType);
	void OnCellSizeSelected(TSharedPtr<int32> Value, ESelectInfo::Type SelectionType);
	void OnEventNameChanged(const FText& Text);
	void OnEventNameCommitted(const FText& Text, ETextCommit::Type CommitType);
	void OnWorldOffsetXChanged(double Value);
	void OnWorldOffsetYChanged(double Value);
	void OnWorldOffsetXCommitted(double Value, ETextCommit::Type CommitType);
	void OnWorldOffsetYCommitted(double Value, ETextCommit::Type CommitType);
	void OnWorldOffsetXSliderEnded(double Value);
	void OnWorldOffsetYSliderEnded(double Value);
	void ApplyWorldOffsetAndSave();
	void OnSettingsObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
	void HandleMapsResponse(
		bool bSuccess,
		uint64 RequestQueryRevision,
		TArray<FramedashEditor::FMapInfo>&& Maps,
		const FString& Error);
	void HandleHeatmapResponse(
		bool bSuccess,
		const FramedashEditor::FMapInfo& Map,
		double CellSize,
		uint64 RequestQueryRevision,
		uint64 RequestOverlayRevision,
		TArray<FramedashEditor::FHeatmapCell>&& Cells,
		const FString& Error);
	void SaveSettings();

	TSharedPtr<FFramedashEditorHttpClient> HttpClient;
	TSharedPtr<FFramedashEditorHeatmapOverlay> Overlay;
	TSharedPtr<SComboBox<TSharedPtr<FramedashEditor::FMapInfo>>> MapComboBox;
	TSharedPtr<SComboBox<TSharedPtr<int32>>> DaysComboBox;
	TSharedPtr<SComboBox<TSharedPtr<int32>>> CellSizeComboBox;
	TSharedPtr<SNumericEntryBox<double>> WorldOffsetXEntry;
	TSharedPtr<SNumericEntryBox<double>> WorldOffsetYEntry;
	TArray<TSharedPtr<FramedashEditor::FMapInfo>> MapOptions;
	TSharedPtr<FramedashEditor::FMapInfo> SelectedMap;
	TArray<TSharedPtr<int32>> DaysOptions;
	TArray<TSharedPtr<int32>> CellSizeOptions;
	TSharedPtr<int32> SelectedDays;
	TSharedPtr<int32> SelectedCellSize;
	FString EventNameFilter;
	FString ObservedReadApiKey;
	FString ObservedApiBaseUrl;
	FString ObservedProjectId;
	FString ObservedEventNameFilter;
	FText StatusText;
	FDelegateHandle SettingsChangedHandle;
	uint64 QueryRevision = 0;
	int32 ObservedDays = 7;
	int32 ObservedCellSize = 25;
	bool bBusy = false;
};
