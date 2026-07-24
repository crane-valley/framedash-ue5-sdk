// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorHeatmapRenderData.h"

#include "FramedashEditorLogic.h"

namespace
{
	constexpr int32 ColorBucketCount = 64;

	constexpr int32 VoxelTriangles[12][3] = {
		{0, 2, 1}, {0, 3, 2},
		{4, 5, 6}, {6, 7, 4},
		{0, 1, 5}, {5, 4, 0},
		{1, 2, 6}, {6, 5, 1},
		{2, 3, 7}, {7, 6, 2},
		{3, 0, 4}, {4, 7, 3},
	};

	constexpr int32 FlatTriangles[2][3] = {
		{0, 1, 2}, {2, 3, 0},
	};
}

namespace FramedashEditor
{
FHeatmapRenderData BuildHeatmapRenderData(
	const TArray<FHeatmapSceneCell>& Cells,
	float Opacity,
	double ZOffset)
{
	FHeatmapRenderData Result;
	Result.CellCount = Cells.Num();
	TMap<int32, int32> BucketSlots;

	for (const auto& Cell : Cells)
	{
		const double SafeWeight = FMath::Clamp(Cell.NormalizedWeight, 0.0, 1.0);
		const int32 BucketIndex = FMath::Clamp(
			FMath::RoundToInt(SafeWeight * (ColorBucketCount - 1)),
			0,
			ColorBucketCount - 1);
		int32* ExistingSlot = BucketSlots.Find(BucketIndex);
		if (ExistingSlot == nullptr)
		{
			const int32 NewSlot = Result.Buckets.AddDefaulted();
			BucketSlots.Add(BucketIndex, NewSlot);
			ExistingSlot = BucketSlots.Find(BucketIndex);
			Result.Buckets[NewSlot].Color = HeatmapColor(
				static_cast<double>(BucketIndex) / (ColorBucketCount - 1),
				Opacity);
			Result.Buckets[NewSlot].ColorBucketIndex = BucketIndex;
		}

		FHeatmapRenderBucket& Bucket = Result.Buckets[*ExistingSlot];
		const int32 CornerCount = Cell.bVolumetric ? 8 : 4;
		const uint32 FirstVertex = static_cast<uint32>(Bucket.Vertices.Num());
		Bucket.Vertices.Reserve(Bucket.Vertices.Num() + CornerCount);
		for (int32 CornerIndex = 0; CornerIndex < CornerCount; ++CornerIndex)
		{
			const FVector Corner =
				Cell.WorldCorners[CornerIndex] + FVector(0.0, 0.0, ZOffset);
			Bucket.Vertices.Add(FVector3f(Corner));
			Result.Bounds += Corner;
		}

		if (Cell.bVolumetric)
		{
			Bucket.Indices.Reserve(Bucket.Indices.Num() + 36);
			for (const auto& Triangle : VoxelTriangles)
			{
				Bucket.Indices.Add(FirstVertex + Triangle[0]);
				Bucket.Indices.Add(FirstVertex + Triangle[1]);
				Bucket.Indices.Add(FirstVertex + Triangle[2]);
			}
			Result.TriangleCount += 12;
		}
		else
		{
			Bucket.Indices.Reserve(Bucket.Indices.Num() + 6);
			for (const auto& Triangle : FlatTriangles)
			{
				Bucket.Indices.Add(FirstVertex + Triangle[0]);
				Bucket.Indices.Add(FirstVertex + Triangle[1]);
				Bucket.Indices.Add(FirstVertex + Triangle[2]);
			}
			Result.TriangleCount += 2;
		}
	}

	Result.Buckets.Sort(
		[](const FHeatmapRenderBucket& Left, const FHeatmapRenderBucket& Right)
		{
			return Left.ColorBucketIndex < Right.ColorBucketIndex;
		});
	return Result;
}
}
