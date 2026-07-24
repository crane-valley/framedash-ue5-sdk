// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorHeatmapVisibility.h"

#include "ShowFlags.h"

#define LOCTEXT_NAMESPACE "FramedashEditorHeatmapVisibility"

namespace
{
	TCustomShowFlag<> FramedashHeatmapShowFlag(
		TEXT("FramedashHeatmap"),
		false,
		SFG_Normal,
		LOCTEXT("FramedashHeatmapShowFlag", "Framedash Heatmap"));
}

namespace FramedashEditor
{
bool IsHeatmapShowFlagEnabled(const FEngineShowFlags& ShowFlags)
{
	return FramedashHeatmapShowFlag.IsEnabled(ShowFlags);
}

bool SetHeatmapShowFlagEnabled(FEngineShowFlags& ShowFlags, bool bEnabled)
{
	const int32 FlagIndex =
		FEngineShowFlags::FindIndexByName(TEXT("FramedashHeatmap"));
	if (FlagIndex < 0)
	{
		return false;
	}
	ShowFlags.SetSingleFlag(static_cast<uint32>(FlagIndex), bEnabled);
	return true;
}

bool ShouldRenderHeatmap(const FEngineShowFlags& ShowFlags, bool bPIESuspended)
{
	return !bPIESuspended && IsHeatmapShowFlagEnabled(ShowFlags);
}
}

#undef LOCTEXT_NAMESPACE
