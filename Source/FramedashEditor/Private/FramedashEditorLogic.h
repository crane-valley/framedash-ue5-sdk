// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace FramedashEditor
{
	struct FMapInfo
	{
		FString Id;
		FString Name;
		FString MapId;
		FString ImageUrl;
		double WorldMinX = 0.0;
		double WorldMinY = 0.0;
		double WorldMaxX = 0.0;
		double WorldMaxY = 0.0;
		TOptional<double> WorldMinZ;
		TOptional<double> WorldMaxZ;
		double ImageWidth = 0.0;
		double ImageHeight = 0.0;
		FString CreatedAt;
		FString UpdatedAt;
	};

	struct FHeatmapCell
	{
		double X = 0.0;
		double Y = 0.0;
		double Weight = 0.0;
		double EventCount = 0.0;
		double AverageFps = 0.0;
		double AverageFrameTime = 0.0;
		double AverageMemory = 0.0;
		TOptional<double> AverageGpuTime;
		TOptional<double> AverageVramMemory;
	};

	struct FCellRect
	{
		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
	};

	bool ParseMapsResponse(const FString& Json, TArray<FMapInfo>& OutMaps, FString& OutError);
	bool ParseHeatmapResponse(const FString& Json, TArray<FHeatmapCell>& OutCells, FString& OutError);
	FString ParseProblemMessage(const FString& Json, const FString& Fallback);

	FCellRect BuildCellRect(const FHeatmapCell& Cell, const FMapInfo& Map, double CellSize);
	double FindMaxWeight(const TArray<FHeatmapCell>& Cells);
	double NormalizeWeight(double Weight, double MaxWeight);
	FLinearColor HeatmapColor(double NormalizedWeight, float Opacity);
}
