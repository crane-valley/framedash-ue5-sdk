// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorHeatmapOverlay.h"

#include "FramedashEditorSettings.h"

#include "BatchedElements.h"
#include "Debug/DebugDrawService.h"
#include "Editor.h"
#include "Engine/Canvas.h"
#include "GlobalRenderResources.h"
#include "SceneView.h"

FFramedashEditorHeatmapOverlay::FFramedashEditorHeatmapOverlay()
{
	BeginPIEHandle = FEditorDelegates::BeginPIE.AddRaw(
		this,
		&FFramedashEditorHeatmapOverlay::OnBeginPIE);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(
		this,
		&FFramedashEditorHeatmapOverlay::OnEndPIE);
}

FFramedashEditorHeatmapOverlay::~FFramedashEditorHeatmapOverlay()
{
	Shutdown();
}

void FFramedashEditorHeatmapOverlay::SetData(
	const FramedashEditor::FMapInfo& Map,
	const TArray<FramedashEditor::FHeatmapCell>& Cells,
	double CellSize,
	const FVector2D& WorldOffset)
{
	ActiveMap = Map;
	ActiveCells = Cells;
	ActiveCellSize = CellSize;
	ActiveWorldOffset = WorldOffset;
	RebuildRenderCells();
}

void FFramedashEditorHeatmapOverlay::SetWorldOffset(const FVector2D& WorldOffset)
{
	if (ActiveWorldOffset.Equals(WorldOffset))
	{
		return;
	}
	ActiveWorldOffset = WorldOffset;
	RebuildRenderCells();
}

void FFramedashEditorHeatmapOverlay::RebuildRenderCells()
{
	RenderCells.Reset();
	RenderCells.Reserve(ActiveCells.Num());
	CachedWorldBounds = FBox(EForceInit::ForceInit);
	// Older 2D API responses fall back to the map floor instead of disappearing.
	BaseZ = ActiveMap.WorldMinZ.Get(0.0);
	const double MaxWeight = FramedashEditor::FindMaxWeight(ActiveCells);
	for (const auto& Cell : ActiveCells)
	{
		FRenderCell& RenderCell = RenderCells.AddDefaulted_GetRef();
		RenderCell.Rect = FramedashEditor::BuildCellRect(
			Cell,
			ActiveMap,
			ActiveCellSize,
			ActiveWorldOffset);
		RenderCell.WorldCorners = FramedashEditor::BuildHeatmapCellCorners(
			RenderCell.Rect,
			Cell,
			BaseZ,
			ActiveCellSize);
		RenderCell.bVolumetric = FramedashEditor::IsVolumetricHeatmapCell(
			Cell,
			ActiveCellSize);
		for (const auto& Corner : RenderCell.WorldCorners)
		{
			CachedWorldBounds += Corner;
		}
		RenderCell.NormalizedWeight = FramedashEditor::NormalizeWeight(Cell.Weight, MaxWeight);
	}
	if (GEditor != nullptr)
	{
		GEditor->RedrawLevelEditingViewports(false);
	}
}

void FFramedashEditorHeatmapOverlay::ClearData()
{
	RenderCells.Reset();
	CachedWorldBounds = FBox(EForceInit::ForceInit);
	ActiveMap = FramedashEditor::FMapInfo{};
	ActiveCells.Reset();
	ActiveCellSize = 0.0;
	ActiveWorldOffset = FVector2D::ZeroVector;
	BaseZ = 0.0;
	if (GEditor != nullptr)
	{
		GEditor->RedrawLevelEditingViewports(false);
	}
}

void FFramedashEditorHeatmapOverlay::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (bEnabled)
	{
		RegisterDrawDelegate();
	}
	else
	{
		UnregisterDrawDelegate();
	}
	if (GEditor != nullptr)
	{
		GEditor->RedrawLevelEditingViewports(false);
	}
}

bool FFramedashEditorHeatmapOverlay::GetWorldBounds(FBox& OutBounds) const
{
	if (!CachedWorldBounds.IsValid)
	{
		return false;
	}

	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	const double ZOffset = Settings != nullptr ? static_cast<double>(Settings->ZOffset) : 0.0;
	OutBounds = FramedashEditor::BuildHeatmapFramingBounds(CachedWorldBounds, ZOffset);
	return true;
}

void FFramedashEditorHeatmapOverlay::Shutdown()
{
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	bEnabled = false;
	UnregisterDrawDelegate();
	FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	BeginPIEHandle.Reset();
	EndPIEHandle.Reset();
	RenderCells.Reset();
	CachedWorldBounds = FBox(EForceInit::ForceInit);
	ActiveCells.Reset();
	if (GEditor != nullptr)
	{
		GEditor->RedrawLevelEditingViewports(false);
	}
}

void FFramedashEditorHeatmapOverlay::RegisterDrawDelegate()
{
	if (bShuttingDown || DrawHandle.IsValid() || (GEditor != nullptr && GEditor->PlayWorld != nullptr))
	{
		return;
	}
	DrawHandle = UDebugDrawService::Register(
		TEXT("Editor"),
		FDebugDrawDelegate::CreateRaw(this, &FFramedashEditorHeatmapOverlay::Draw));
}

void FFramedashEditorHeatmapOverlay::UnregisterDrawDelegate()
{
	if (!DrawHandle.IsValid())
	{
		return;
	}
	UDebugDrawService::Unregister(DrawHandle);
	DrawHandle.Reset();
}

void FFramedashEditorHeatmapOverlay::Draw(UCanvas* Canvas, APlayerController*)
{
	if (!bEnabled || bShuttingDown || Canvas == nullptr || Canvas->Canvas == nullptr ||
		Canvas->SceneView == nullptr || RenderCells.IsEmpty() ||
		(GEditor != nullptr && GEditor->PlayWorld != nullptr))
	{
		return;
	}

	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	if (Settings == nullptr)
	{
		return;
	}
	const float Opacity = FMath::Clamp(Settings->OverlayOpacity, 0.0f, 1.0f);
	const FVector ZOffset(0.0, 0.0, static_cast<double>(Settings->ZOffset));

	FBatchedElements* Triangles = Canvas->Canvas->GetBatchedElements(
		FCanvas::ET_Triangle,
		nullptr,
		GWhiteTexture,
		SE_BLEND_Translucent);
	if (Triangles == nullptr)
	{
		return;
	}
	Triangles->ReserveVertices(RenderCells.Num() * 8);
	Triangles->AddReserveTriangles(RenderCells.Num() * 12, GWhiteTexture, SE_BLEND_Translucent);
	const FHitProxyId HitProxyId = Canvas->Canvas->GetHitProxyId();
	static const int32 VoxelTriangles[12][3] = {
		{0, 2, 1}, {0, 3, 2},
		{4, 5, 6}, {6, 7, 4},
		{0, 1, 5}, {5, 4, 0},
		{1, 2, 6}, {6, 5, 1},
		{2, 3, 7}, {7, 6, 2},
		{3, 0, 4}, {4, 7, 3},
	};
	static const int32 FlatTriangles[2][3] = {
		{0, 1, 2}, {2, 3, 0},
	};

	for (const auto& Cell : RenderCells)
	{
		FVector2D ScreenCorners[8];
		bool bBehindView = false;
		const int32 CornerCount = Cell.bVolumetric ? 8 : 4;
		for (int32 CornerIndex = 0; CornerIndex < CornerCount; ++CornerIndex)
		{
			const FVector WorldCorner = Cell.WorldCorners[CornerIndex] + ZOffset;
			if (Canvas->SceneView->Project(WorldCorner).W <= 0.0)
			{
				bBehindView = true;
				break;
			}
			const FVector Screen = Canvas->Project(WorldCorner, false);
			ScreenCorners[CornerIndex] = FVector2D(Screen.X, Screen.Y);
		}
		if (bBehindView)
		{
			continue;
		}

		const FLinearColor Color = FramedashEditor::HeatmapColor(Cell.NormalizedWeight, Opacity);
		int32 VertexIndices[8];
		for (int32 CornerIndex = 0; CornerIndex < CornerCount; ++CornerIndex)
		{
			VertexIndices[CornerIndex] = Triangles->AddVertexf(
				FVector4f(
					static_cast<float>(ScreenCorners[CornerIndex].X),
					static_cast<float>(ScreenCorners[CornerIndex].Y),
					0.0f,
					1.0f),
				FVector2f::ZeroVector,
				Color,
				HitProxyId);
		}
		if (Cell.bVolumetric)
		{
			for (const auto& Triangle : VoxelTriangles)
			{
				Triangles->AddTriangle(
					VertexIndices[Triangle[0]],
					VertexIndices[Triangle[1]],
					VertexIndices[Triangle[2]],
					GWhiteTexture,
					SE_BLEND_Translucent);
			}
		}
		else
		{
			for (const auto& Triangle : FlatTriangles)
			{
				Triangles->AddTriangle(
					VertexIndices[Triangle[0]],
					VertexIndices[Triangle[1]],
					VertexIndices[Triangle[2]],
					GWhiteTexture,
					SE_BLEND_Translucent);
			}
		}
	}
}

void FFramedashEditorHeatmapOverlay::OnBeginPIE(bool)
{
	UnregisterDrawDelegate();
}

void FFramedashEditorHeatmapOverlay::OnEndPIE(bool)
{
	if (bEnabled)
	{
		RegisterDrawDelegate();
	}
}
