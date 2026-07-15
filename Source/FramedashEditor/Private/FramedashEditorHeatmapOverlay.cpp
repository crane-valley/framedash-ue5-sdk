// Copyright Crane Valley. All Rights Reserved.

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
	double CellSize)
{
	RenderCells.Reset();
	RenderCells.Reserve(Cells.Num());
	// Maps without a recorded Z floor render at world origin height instead of disappearing.
	BaseZ = Map.WorldMinZ.Get(0.0);
	const double MaxWeight = FramedashEditor::FindMaxWeight(Cells);
	for (const FramedashEditor::FHeatmapCell& Cell : Cells)
	{
		FRenderCell& RenderCell = RenderCells.AddDefaulted_GetRef();
		RenderCell.Rect = FramedashEditor::BuildCellRect(Cell, Map, CellSize);
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
	const double DrawZ = BaseZ + static_cast<double>(Settings->ZOffset);
	const float Opacity = FMath::Clamp(Settings->OverlayOpacity, 0.0f, 1.0f);

	FBatchedElements* Triangles = Canvas->Canvas->GetBatchedElements(
		FCanvas::ET_Triangle,
		nullptr,
		GWhiteTexture,
		SE_BLEND_Translucent);
	if (Triangles == nullptr)
	{
		return;
	}
	Triangles->ReserveVertices(RenderCells.Num() * 4);
	Triangles->AddReserveTriangles(RenderCells.Num() * 2, GWhiteTexture, SE_BLEND_Translucent);
	const FHitProxyId HitProxyId = Canvas->Canvas->GetHitProxyId();

	for (const FRenderCell& Cell : RenderCells)
	{
		const FVector WorldCorners[4] = {
			FVector(Cell.Rect.MinX, Cell.Rect.MinY, DrawZ),
			FVector(Cell.Rect.MaxX, Cell.Rect.MinY, DrawZ),
			FVector(Cell.Rect.MaxX, Cell.Rect.MaxY, DrawZ),
			FVector(Cell.Rect.MinX, Cell.Rect.MaxY, DrawZ),
		};

		FVector2D ScreenCorners[4];
		bool bBehindView = false;
		for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
		{
			if (Canvas->SceneView->Project(WorldCorners[CornerIndex]).W <= 0.0)
			{
				bBehindView = true;
				break;
			}
			const FVector Screen = Canvas->Project(WorldCorners[CornerIndex], false);
			ScreenCorners[CornerIndex] = FVector2D(Screen.X, Screen.Y);
		}
		if (bBehindView)
		{
			continue;
		}

		const FLinearColor Color = FramedashEditor::HeatmapColor(Cell.NormalizedWeight, Opacity);
		const int32 Vertex0 = Triangles->AddVertexf(
			FVector4f(static_cast<float>(ScreenCorners[0].X), static_cast<float>(ScreenCorners[0].Y), 0.0f, 1.0f),
			FVector2f::ZeroVector,
			Color,
			HitProxyId);
		const int32 Vertex1 = Triangles->AddVertexf(
			FVector4f(static_cast<float>(ScreenCorners[1].X), static_cast<float>(ScreenCorners[1].Y), 0.0f, 1.0f),
			FVector2f::ZeroVector,
			Color,
			HitProxyId);
		const int32 Vertex2 = Triangles->AddVertexf(
			FVector4f(static_cast<float>(ScreenCorners[2].X), static_cast<float>(ScreenCorners[2].Y), 0.0f, 1.0f),
			FVector2f::ZeroVector,
			Color,
			HitProxyId);
		const int32 Vertex3 = Triangles->AddVertexf(
			FVector4f(static_cast<float>(ScreenCorners[3].X), static_cast<float>(ScreenCorners[3].Y), 0.0f, 1.0f),
			FVector2f::ZeroVector,
			Color,
			HitProxyId);
		Triangles->AddTriangle(Vertex0, Vertex1, Vertex2, GWhiteTexture, SE_BLEND_Translucent);
		Triangles->AddTriangle(Vertex2, Vertex3, Vertex0, GWhiteTexture, SE_BLEND_Translucent);
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
