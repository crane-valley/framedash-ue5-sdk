// Copyright Crane Valley. All Rights Reserved.

#include "FramedashEditorSettings.h"

UFramedashEditorSettings::UFramedashEditorSettings()
	: ApiBaseUrl(TEXT("https://app.framedash.dev"))
	, Days(7)
	, CellSize(25)
	, OverlayOpacity(0.6f)
	, ZOffset(0.0f)
{
}

FText UFramedashEditorSettings::GetSectionText() const
{
	return NSLOCTEXT("FramedashEditorSettings", "SectionText", "Framedash Heatmap");
}
