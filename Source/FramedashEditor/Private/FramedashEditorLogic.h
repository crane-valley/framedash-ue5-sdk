// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace FramedashEditor
{
	FString ResolveReadApiKey(const FString& ConfiguredValue, const FString& EnvironmentValue);

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
		TOptional<double> Z;
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

	FCellRect BuildCellRect(
		const FHeatmapCell& Cell,
		const FMapInfo& Map,
		double CellSize,
		const FVector2D& WorldOffset = FVector2D::ZeroVector);
	double FindMaxWeight(const TArray<FHeatmapCell>& Cells);
	double NormalizeWeight(double Weight, double MaxWeight);
	TStaticArray<FVector, 8> BuildVoxelCorners(
		const FCellRect& Rect,
		double CenterZ,
		double CellSize);
	TStaticArray<FVector, 8> BuildHeatmapCellCorners(
		const FCellRect& Rect,
		const FHeatmapCell& Cell,
		double BaseZ,
		double CellSize);
	bool IsVolumetricHeatmapCell(const FHeatmapCell& Cell, double CellSize);
	FBox BuildHeatmapFramingBounds(const FBox& CellBounds, double ZOffset = 0.0);
	FLinearColor HeatmapColor(double NormalizedWeight, float Opacity);
}
