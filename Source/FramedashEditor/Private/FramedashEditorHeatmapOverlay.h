// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

#include "FramedashEditorHeatmapRenderData.h"
#include "FramedashEditorLogic.h"

class UFramedashEditorHeatmapComponent;
class UObject;
class UWorld;
struct FPropertyChangedEvent;

class FFramedashEditorHeatmapOverlay
{
public:
	FFramedashEditorHeatmapOverlay();
	~FFramedashEditorHeatmapOverlay();

	void SetData(
		const FramedashEditor::FMapInfo& Map,
		const TArray<FramedashEditor::FHeatmapCell>& Cells,
		double CellSize,
		const FVector2D& WorldOffset = FVector2D::ZeroVector);
	void SetWorldOffset(const FVector2D& WorldOffset);
	void ClearData();
	bool GetWorldBounds(FBox& OutBounds) const;
	uint64 GetDataRevision() const { return DataRevision; }
	bool IsDataRevisionCurrent(uint64 Revision) const
	{
		return Revision == DataRevision;
	}
	void Shutdown();

#if WITH_DEV_AUTOMATION_TESTS
	UFramedashEditorHeatmapComponent* GetRenderComponentForTesting() const
	{
		return RenderComponent.Get();
	}
#endif

private:
	void RebuildRenderCells();
	void RefreshRenderComponent();
	void EnsureRenderComponentRegistered();
	void ReleaseRenderComponent();
	void CaptureQuerySettings();
	void OnSettingsObjectPropertyChanged(
		UObject* Object,
		FPropertyChangedEvent& PropertyChangedEvent);
	void OnMapChanged(uint32 MapChangeFlags);
	void OnWorldCleanup(
		UWorld* World,
		bool bSessionEnded,
		bool bCleanupResources);
	void OnBeginPIE(bool bIsSimulating);
	void OnEndPIE(bool bIsSimulating);

	TArray<FramedashEditor::FHeatmapSceneCell> RenderCells;
	FBox CachedWorldBounds = FBox(EForceInit::ForceInit);
	FramedashEditor::FMapInfo ActiveMap;
	TArray<FramedashEditor::FHeatmapCell> ActiveCells;
	TStrongObjectPtr<UFramedashEditorHeatmapComponent> RenderComponent;
	FString ObservedReadApiKey;
	FString ObservedApiBaseUrl;
	FString ObservedProjectId;
	FString ObservedEventNameFilter;
	double ActiveCellSize = 0.0;
	FVector2D ActiveWorldOffset = FVector2D::ZeroVector;
	double BaseZ = 0.0;
	uint64 DataRevision = 0;
	int32 ObservedDays = 7;
	int32 ObservedCellSize = 25;
	bool bShuttingDown = false;
	bool bPIESuspended = false;
	FDelegateHandle SettingsChangedHandle;
	FDelegateHandle MapChangedHandle;
	FDelegateHandle WorldCleanupHandle;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
};
