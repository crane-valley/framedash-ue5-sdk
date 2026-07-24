// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorHeatmapComponent.h"

#include "FramedashEditorHeatmapVisibility.h"

#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "SceneManagement.h"
#include "SceneView.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(FramedashEditorHeatmapComponent)

namespace
{
	class FFramedashEditorHeatmapSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		explicit FFramedashEditorHeatmapSceneProxy(
			const UFramedashEditorHeatmapComponent* Component,
			const FramedashEditor::FHeatmapRenderData& InRenderData)
			: FPrimitiveSceneProxy(Component)
			, RenderData(InRenderData)
		{
			bWillEverBeLit = false;
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual void GetDynamicMeshElements(
			const TArray<const FSceneView*>& Views,
			const FSceneViewFamily&,
			uint32 VisibilityMap,
			FMeshElementCollector& Collector) const override
		{
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				const FSceneView* View = Views[ViewIndex];
				if ((VisibilityMap & (1 << ViewIndex)) == 0 ||
					View == nullptr ||
					View->Family == nullptr ||
					GEngine == nullptr ||
					GEngine->DebugMeshMaterial == nullptr ||
					!FramedashEditor::ShouldRenderHeatmap(
						View->Family->EngineShowFlags,
						false))
				{
					continue;
				}

				for (const auto& Bucket : RenderData.Buckets)
				{
					FDynamicMeshBuilder MeshBuilder(View->GetFeatureLevel());
					MeshBuilder.ReserveVertices(Bucket.Vertices.Num());
					MeshBuilder.ReserveTriangles(Bucket.Indices.Num() / 3);
					for (const auto& Vertex : Bucket.Vertices)
					{
						MeshBuilder.AddVertex(
							Vertex,
							FVector2f::ZeroVector,
							FVector3f(1.0f, 0.0f, 0.0f),
							FVector3f(0.0f, 1.0f, 0.0f),
							FVector3f(0.0f, 0.0f, 1.0f),
							FColor::White);
					}
					for (int32 Index = 0; Index < Bucket.Indices.Num(); Index += 3)
					{
						MeshBuilder.AddTriangle(
							Bucket.Indices[Index],
							Bucket.Indices[Index + 1],
							Bucket.Indices[Index + 2]);
					}

					FMaterialRenderProxy* MaterialProxy =
						new FColoredMaterialRenderProxy(
							GEngine->DebugMeshMaterial->GetRenderProxy(),
							Bucket.Color);
					Collector.RegisterOneFrameMaterialProxy(MaterialProxy);
					MeshBuilder.GetMesh(
						FMatrix::Identity,
						MaterialProxy,
						SDPG_World,
						false,
						false,
						ViewIndex,
						Collector);
				}
			}
		}

		virtual FPrimitiveViewRelevance GetViewRelevance(
			const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Relevance;
			const bool bShouldRender =
				View != nullptr &&
				View->Family != nullptr &&
				IsShown(View) &&
				FramedashEditor::ShouldRenderHeatmap(
					View->Family->EngineShowFlags,
					false);
			FramedashEditor::ConfigureHeatmapViewRelevance(
				Relevance,
				bShouldRender);
			return Relevance;
		}

		virtual uint32 GetMemoryFootprint() const override
		{
			return sizeof(*this) + GetAllocatedSize();
		}

	private:
		uint32 GetAllocatedSize() const
		{
			uint32 Size = FPrimitiveSceneProxy::GetAllocatedSize() +
				RenderData.Buckets.GetAllocatedSize();
			for (const auto& Bucket : RenderData.Buckets)
			{
				Size += Bucket.Vertices.GetAllocatedSize();
				Size += Bucket.Indices.GetAllocatedSize();
			}
			return Size;
		}

		FramedashEditor::FHeatmapRenderData RenderData;
	};
}

void FramedashEditor::ConfigureHeatmapViewRelevance(
	FPrimitiveViewRelevance& Relevance,
	bool bShouldRender)
{
	Relevance.bDrawRelevance = bShouldRender;
	Relevance.bDynamicRelevance = true;
	Relevance.bRenderInMainPass = true;
	Relevance.bNormalTranslucency = true;
	Relevance.bSeparateTranslucency = true;
}

UFramedashEditorHeatmapComponent::UFramedashEditorHeatmapComponent()
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	bUseEditorCompositing = false;
	bIgnoreStreamingManagerUpdate = true;
}

void UFramedashEditorHeatmapComponent::SetRenderData(
	const FramedashEditor::FHeatmapRenderData& InRenderData)
{
	RenderData = InRenderData;
	UpdateBounds();
	MarkRenderStateDirty();
}

void UFramedashEditorHeatmapComponent::SetPIESuspended(bool bInPIESuspended)
{
	if (bPIESuspended == bInPIESuspended)
	{
		return;
	}
	bPIESuspended = bInPIESuspended;
	MarkRenderStateDirty();
}

bool UFramedashEditorHeatmapComponent::IsPIESuspended() const
{
	return bPIESuspended;
}

FPrimitiveSceneProxy* UFramedashEditorHeatmapComponent::CreateSceneProxy()
{
	if (bPIESuspended || RenderData.CellCount == 0 || RenderData.Buckets.IsEmpty())
	{
		return nullptr;
	}
	return new FFramedashEditorHeatmapSceneProxy(this, RenderData);
}

FBoxSphereBounds UFramedashEditorHeatmapComponent::CalcBounds(
	const FTransform& LocalToWorld) const
{
	const FBox LocalBounds = RenderData.Bounds.IsValid
		? RenderData.Bounds
		: FBox(FVector(-1.0), FVector(1.0));
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
}
