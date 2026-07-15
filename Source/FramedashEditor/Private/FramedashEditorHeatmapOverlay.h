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
		double CellSize);
	void ClearData();
	void SetEnabled(bool bEnabled);
	void Shutdown();

private:
	struct FRenderCell
	{
		FramedashEditor::FCellRect Rect;
		double NormalizedWeight = 0.0;
	};

	void RegisterDrawDelegate();
	void UnregisterDrawDelegate();
	void Draw(UCanvas* Canvas, APlayerController* PlayerController);
	void OnBeginPIE(bool bIsSimulating);
	void OnEndPIE(bool bIsSimulating);

	TArray<FRenderCell> RenderCells;
	double BaseZ = 0.0;
	bool bEnabled = false;
	bool bShuttingDown = false;
	FDelegateHandle DrawHandle;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
};
