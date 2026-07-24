// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "Components/PrimitiveComponent.h"

#include "FramedashEditorHeatmapRenderData.h"

#include "FramedashEditorHeatmapComponent.generated.h"

struct FPrimitiveViewRelevance;

namespace FramedashEditor
{
	void ConfigureHeatmapViewRelevance(
		FPrimitiveViewRelevance& Relevance,
		bool bShouldRender);
}

UCLASS(Transient, NotBlueprintable, NotPlaceable)
class UFramedashEditorHeatmapComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UFramedashEditorHeatmapComponent();

	void SetRenderData(const FramedashEditor::FHeatmapRenderData& InRenderData);
	void SetPIESuspended(bool bInPIESuspended);
	bool IsPIESuspended() const;

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	FramedashEditor::FHeatmapRenderData RenderData;
	bool bPIESuspended = false;
};
