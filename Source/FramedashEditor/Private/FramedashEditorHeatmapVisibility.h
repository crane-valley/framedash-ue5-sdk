// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FEngineShowFlags;

namespace FramedashEditor
{
bool IsHeatmapShowFlagEnabled(const FEngineShowFlags& ShowFlags);
bool SetHeatmapShowFlagEnabled(FEngineShowFlags& ShowFlags, bool bEnabled);
bool ShouldRenderHeatmap(const FEngineShowFlags& ShowFlags, bool bPIESuspended);
}
