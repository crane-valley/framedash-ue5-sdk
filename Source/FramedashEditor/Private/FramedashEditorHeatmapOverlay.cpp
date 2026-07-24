// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorHeatmapOverlay.h"

#include "FramedashEditorHeatmapComponent.h"
#include "FramedashEditorSettings.h"

#include "Editor.h"
#include "Engine/World.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

FFramedashEditorHeatmapOverlay::FFramedashEditorHeatmapOverlay()
{
	CaptureQuerySettings();
	SettingsChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this,
		&FFramedashEditorHeatmapOverlay::OnSettingsObjectPropertyChanged);
	MapChangedHandle = FEditorDelegates::MapChange.AddRaw(
		this,
		&FFramedashEditorHeatmapOverlay::OnMapChanged);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this,
		&FFramedashEditorHeatmapOverlay::OnWorldCleanup);
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
	BaseZ = ActiveMap.WorldMinZ.Get(0.0);
	const double MaxWeight = FramedashEditor::FindMaxWeight(ActiveCells);
	for (const auto& Cell : ActiveCells)
	{
		FramedashEditor::FHeatmapSceneCell& RenderCell =
			RenderCells.AddDefaulted_GetRef();
		const FramedashEditor::FCellRect Rect = FramedashEditor::BuildCellRect(
			Cell,
			ActiveMap,
			ActiveCellSize,
			ActiveWorldOffset);
		RenderCell.WorldCorners = FramedashEditor::BuildHeatmapCellCorners(
			Rect,
			Cell,
			BaseZ,
			ActiveCellSize);
		RenderCell.bVolumetric = FramedashEditor::IsVolumetricHeatmapCell(
			Cell,
			ActiveCellSize);
		const int32 CornerCount = RenderCell.bVolumetric ? 8 : 4;
		for (int32 CornerIndex = 0; CornerIndex < CornerCount; ++CornerIndex)
		{
			CachedWorldBounds += RenderCell.WorldCorners[CornerIndex];
		}
		RenderCell.NormalizedWeight =
			FramedashEditor::NormalizeWeight(Cell.Weight, MaxWeight);
	}
	RefreshRenderComponent();
}

void FFramedashEditorHeatmapOverlay::RefreshRenderComponent()
{
	if (bShuttingDown)
	{
		return;
	}
	if (RenderComponent.Get() == nullptr)
	{
		if (RenderCells.IsEmpty())
		{
			return;
		}
		RenderComponent.Reset(NewObject<UFramedashEditorHeatmapComponent>(
			GetTransientPackage(),
			NAME_None,
			RF_Transient));
		RenderComponent->SetPIESuspended(
			bPIESuspended ||
			(GEditor != nullptr && GEditor->PlayWorld != nullptr));
	}

	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	const float Opacity = Settings != nullptr
		? FMath::Clamp(Settings->OverlayOpacity, 0.0f, 1.0f)
		: 0.6f;
	const double ZOffset = Settings != nullptr
		? static_cast<double>(Settings->ZOffset)
		: 0.0;
	RenderComponent->SetRenderData(
		FramedashEditor::BuildHeatmapRenderData(
			RenderCells,
			Opacity,
			ZOffset));
	EnsureRenderComponentRegistered();
	if (GEditor != nullptr)
	{
		GEditor->RedrawLevelEditingViewports(false);
	}
}

void FFramedashEditorHeatmapOverlay::EnsureRenderComponentRegistered()
{
	if (bShuttingDown || RenderComponent.Get() == nullptr || GEditor == nullptr)
	{
		return;
	}

	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (EditorWorld == nullptr)
	{
		return;
	}
	if (!RenderComponent->IsRegistered())
	{
		RenderComponent->RegisterComponentWithWorld(EditorWorld);
	}
}

void FFramedashEditorHeatmapOverlay::ReleaseRenderComponent()
{
	if (RenderComponent.Get() == nullptr)
	{
		return;
	}
	if (RenderComponent->IsRegistered())
	{
		RenderComponent->UnregisterComponent();
	}
	RenderComponent.Reset();
}

void FFramedashEditorHeatmapOverlay::CaptureQuerySettings()
{
	if (const UFramedashEditorSettings* Settings =
		GetDefault<UFramedashEditorSettings>())
	{
		ObservedReadApiKey = Settings->ReadApiKey;
		ObservedApiBaseUrl = Settings->ApiBaseUrl;
		ObservedProjectId = Settings->ProjectId;
		ObservedEventNameFilter = Settings->EventNameFilter;
		ObservedDays = Settings->Days;
		ObservedCellSize = Settings->CellSize;
	}
}

void FFramedashEditorHeatmapOverlay::ClearData()
{
	++DataRevision;
	CaptureQuerySettings();
	ReleaseRenderComponent();
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

bool FFramedashEditorHeatmapOverlay::GetWorldBounds(FBox& OutBounds) const
{
	if (!CachedWorldBounds.IsValid)
	{
		return false;
	}

	const UFramedashEditorSettings* Settings = GetDefault<UFramedashEditorSettings>();
	const double ZOffset = Settings != nullptr
		? static_cast<double>(Settings->ZOffset)
		: 0.0;
	OutBounds = FramedashEditor::BuildHeatmapFramingBounds(
		CachedWorldBounds,
		ZOffset);
	return true;
}

void FFramedashEditorHeatmapOverlay::Shutdown()
{
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(SettingsChangedHandle);
	FEditorDelegates::MapChange.Remove(MapChangedHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	SettingsChangedHandle.Reset();
	MapChangedHandle.Reset();
	WorldCleanupHandle.Reset();
	BeginPIEHandle.Reset();
	EndPIEHandle.Reset();
	ReleaseRenderComponent();
	RenderCells.Reset();
	CachedWorldBounds = FBox(EForceInit::ForceInit);
	ActiveCells.Reset();
	if (GEditor != nullptr)
	{
		GEditor->RedrawLevelEditingViewports(false);
	}
}

void FFramedashEditorHeatmapOverlay::OnSettingsObjectPropertyChanged(
	UObject* Object,
	FPropertyChangedEvent& PropertyChangedEvent)
{
	const UFramedashEditorSettings* Settings =
		GetDefault<UFramedashEditorSettings>();
	if (Settings == nullptr || Object != Settings)
	{
		return;
	}

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const bool bCheckAllProperties = PropertyName.IsNone();
	const bool bReadApiKeyProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, ReadApiKey);
	const bool bApiBaseUrlProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, ApiBaseUrl);
	const bool bProjectIdProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, ProjectId);
	const bool bDaysProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, Days);
	const bool bCellSizeProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, CellSize);
	const bool bEventNameProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, EventNameFilter);

	const bool bQuerySettingsChanged =
		(bReadApiKeyProperty && ObservedReadApiKey != Settings->ReadApiKey) ||
		(bApiBaseUrlProperty && ObservedApiBaseUrl != Settings->ApiBaseUrl) ||
		(bProjectIdProperty && ObservedProjectId != Settings->ProjectId) ||
		(bDaysProperty && ObservedDays != Settings->Days) ||
		(bCellSizeProperty && ObservedCellSize != Settings->CellSize) ||
		(bEventNameProperty &&
			ObservedEventNameFilter != Settings->EventNameFilter);
	if (bReadApiKeyProperty)
	{
		ObservedReadApiKey = Settings->ReadApiKey;
	}
	if (bApiBaseUrlProperty)
	{
		ObservedApiBaseUrl = Settings->ApiBaseUrl;
	}
	if (bProjectIdProperty)
	{
		ObservedProjectId = Settings->ProjectId;
	}
	if (bDaysProperty)
	{
		ObservedDays = Settings->Days;
	}
	if (bCellSizeProperty)
	{
		ObservedCellSize = Settings->CellSize;
	}
	if (bEventNameProperty)
	{
		ObservedEventNameFilter = Settings->EventNameFilter;
	}
	if (bQuerySettingsChanged)
	{
		ClearData();
		return;
	}

	const bool bWorldOffsetXProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, WorldOffsetX);
	const bool bWorldOffsetYProperty = bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, WorldOffsetY);
	if (bWorldOffsetXProperty || bWorldOffsetYProperty)
	{
		SetWorldOffset(FVector2D(
			Settings->WorldOffsetX,
			Settings->WorldOffsetY));
	}

	if (bCheckAllProperties ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, OverlayOpacity) ||
		PropertyName ==
			GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, ZOffset))
	{
		RefreshRenderComponent();
	}
}

void FFramedashEditorHeatmapOverlay::OnMapChanged(uint32)
{
	ClearData();
}

void FFramedashEditorHeatmapOverlay::OnWorldCleanup(
	UWorld* World,
	bool,
	bool)
{
	if (RenderComponent.Get() != nullptr &&
		RenderComponent->IsRegistered() &&
		RenderComponent->GetWorld() == World)
	{
		ReleaseRenderComponent();
	}
}

void FFramedashEditorHeatmapOverlay::OnBeginPIE(bool)
{
	bPIESuspended = true;
	if (RenderComponent.Get() != nullptr)
	{
		RenderComponent->SetPIESuspended(true);
	}
}

void FFramedashEditorHeatmapOverlay::OnEndPIE(bool)
{
	bPIESuspended = false;
	EnsureRenderComponentRegistered();
	if (RenderComponent.Get() != nullptr)
	{
		RenderComponent->SetPIESuspended(false);
	}
}
