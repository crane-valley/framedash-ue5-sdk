// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "FramedashEditorLogic.h"

class APlayerController;
class UCanvas;

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
	void SetEnabled(bool bEnabled);
	bool GetWorldBounds(FBox& OutBounds) const;
	void Shutdown();

private:
	struct FRenderCell
	{
		FramedashEditor::FCellRect Rect;
		TStaticArray<FVector, 8> WorldCorners;
		double NormalizedWeight = 0.0;
		bool bVolumetric = false;
	};

	void RebuildRenderCells();
	void RegisterDrawDelegate();
	void UnregisterDrawDelegate();
	void Draw(UCanvas* Canvas, APlayerController* PlayerController);
	void OnBeginPIE(bool bIsSimulating);
	void OnEndPIE(bool bIsSimulating);

	TArray<FRenderCell> RenderCells;
	FBox CachedWorldBounds = FBox(EForceInit::ForceInit);
	FramedashEditor::FMapInfo ActiveMap;
	TArray<FramedashEditor::FHeatmapCell> ActiveCells;
	double ActiveCellSize = 0.0;
	FVector2D ActiveWorldOffset = FVector2D::ZeroVector;
	double BaseZ = 0.0;
	bool bEnabled = false;
	bool bShuttingDown = false;
	FDelegateHandle DrawHandle;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
};
