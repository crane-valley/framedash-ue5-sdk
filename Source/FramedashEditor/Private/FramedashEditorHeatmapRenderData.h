// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace FramedashEditor
{
struct FHeatmapSceneCell
{
	TStaticArray<FVector, 8> WorldCorners;
	double NormalizedWeight = 0.0;
	bool bVolumetric = false;
};

struct FHeatmapRenderBucket
{
	FLinearColor Color;
	int32 ColorBucketIndex = 0;
	TArray<FVector3f> Vertices;
	TArray<uint32> Indices;
};

struct FHeatmapRenderData
{
	TArray<FHeatmapRenderBucket> Buckets;
	FBox Bounds = FBox(EForceInit::ForceInit);
	int32 CellCount = 0;
	int32 TriangleCount = 0;
};

FHeatmapRenderData BuildHeatmapRenderData(
	const TArray<FHeatmapSceneCell>& Cells,
	float Opacity,
	double ZOffset);
}
