// Copyright 2026 Crane Valley. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_EDITOR_WITH_AUTOMATION_SPECS

#include "Misc/AutomationTest.h"

#include "FramedashEditor.h"
#include "FramedashEditorEndpointSecurity.h"
#include "FramedashEditorHeatmapComponent.h"
#include "FramedashEditorHeatmapLayout.h"
#include "FramedashEditorHeatmapOverlay.h"
#include "FramedashEditorHeatmapPanel.h"
#include "FramedashEditorHeatmapRenderData.h"
#include "FramedashEditorHeatmapVisibility.h"
#include "FramedashEditorLogic.h"
#include "FramedashEditorSettings.h"
#include "Modules/ModuleManager.h"
#include "ShowFlags.h"
#include "PrimitiveViewRelevance.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"

BEGIN_DEFINE_SPEC(FFramedashEditorHeatmapSpec, "Framedash.Editor.Heatmap.Logic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FFramedashEditorHeatmapSpec)

void FFramedashEditorHeatmapSpec::Define()
{
	Describe("editor integration", [this]()
	{
		It("registers one nested Window menu command", [this]()
		{
			UToolMenu* WindowMenu =
				UToolMenus::Get()->FindMenu(TEXT("LevelEditor.MainMenu.Window"));
			UToolMenu* FramedashMenu =
				UToolMenus::Get()->FindMenu(
					TEXT("LevelEditor.MainMenu.Window.Framedash"));

			TestNotNull(TEXT("Window menu exists"), WindowMenu);
			TestNotNull(TEXT("Framedash submenu exists"), FramedashMenu);
			if (WindowMenu != nullptr)
			{
				TestTrue(
					TEXT("Window contains one Framedash submenu entry"),
					WindowMenu->ContainsEntry(TEXT("Framedash")));
				TestFalse(
					TEXT("heatmap command is not duplicated at Window root"),
					WindowMenu->ContainsEntry(TEXT("OpenFramedashHeatmap")));
			}
			if (FramedashMenu != nullptr)
			{
				TestTrue(
					TEXT("nested submenu contains the heatmap command"),
					FramedashMenu->ContainsEntry(TEXT("OpenFramedashHeatmap")));
			}
		});

		It("keeps the map selector usable in a narrow dock", [this]()
		{
			TestTrue(
				TEXT("map selector has a compact minimum width"),
				FramedashEditor::HeatmapMapSelectorMinimumWidth >= 180.0f);
		});

		It("keeps the overlay alive when the data panel closes", [this]()
		{
			FFramedashEditorModule& Module =
				FModuleManager::GetModuleChecked<FFramedashEditorModule>(
					TEXT("FramedashEditor"));
			const TSharedPtr<FFramedashEditorHeatmapOverlay> Overlay =
				Module.GetHeatmapOverlayForTesting();
			FramedashEditor::FMapInfo Map;
			Map.WorldMaxX = 100.0;
			Map.WorldMaxY = 100.0;
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FramedashEditor::FHeatmapCell& Cell = Cells.AddDefaulted_GetRef();
			Cell.X = 12.5;
			Cell.Y = 12.5;
			Overlay->SetData(Map, Cells, 25.0);
			TSharedPtr<SFramedashEditorHeatmapPanel> Panel =
				SNew(SFramedashEditorHeatmapPanel)
				.Overlay(Overlay);
			FBox Bounds;

			TestTrue(TEXT("module owns the overlay before panel closure"),
				Module.HasHeatmapOverlayForTesting());
			TestTrue(
				TEXT("opening controls preserves loaded overlay data"),
				Overlay->GetWorldBounds(Bounds));
			Panel.Reset();
			TestTrue(TEXT("panel closure does not destroy the overlay"),
				Module.HasHeatmapOverlayForTesting() && Overlay.IsValid());

			Panel =
				SNew(SFramedashEditorHeatmapPanel)
				.Overlay(Overlay);
			TestTrue(
				TEXT("reopening controls preserves loaded overlay data"),
				Overlay->GetWorldBounds(Bounds));
			Panel.Reset();
			Overlay->ClearData();
		});
	});

	Describe("read API key resolution", [this]()
	{
		It("prefers the configured key and trims it", [this]()
		{
			TestEqual(
				TEXT("configured key wins"),
				FramedashEditor::ResolveReadApiKey(TEXT(" configured-key "), TEXT("environment-key")),
				FString(TEXT("configured-key")));
		});

		It("uses the process environment value when no key is configured", [this]()
		{
			TestEqual(
				TEXT("environment fallback"),
				FramedashEditor::ResolveReadApiKey(TEXT("  "), TEXT(" environment-key ")),
				FString(TEXT("environment-key")));
		});
	});

	Describe("REST response parsing", [this]()
	{
		It("parses a valid maps response", [this]()
		{
			const FString Json = TEXT(R"JSON({"success":true,"data":[{"id":"11111111-1111-1111-1111-111111111111","name":"Arena","mapId":"arena","imageUrl":"https://example.test/map.png","worldMinX":-100,"worldMinY":-50,"worldMaxX":100,"worldMaxY":75,"worldMinZ":10,"worldMaxZ":200,"imageWidth":1024,"imageHeight":512,"createdAt":"2026-07-01T00:00:00Z","updatedAt":"2026-07-02T00:00:00Z"}]})JSON");
			TArray<FramedashEditor::FMapInfo> Maps;
			FString Error;

			TestTrue(TEXT("valid maps response accepted"), FramedashEditor::ParseMapsResponse(Json, Maps, Error));
			TestEqual(TEXT("one map parsed"), Maps.Num(), 1);
			if (Maps.Num() == 1)
			{
				TestEqual(TEXT("map id"), Maps[0].Id, FString(TEXT("11111111-1111-1111-1111-111111111111")));
				TestEqual(TEXT("map slug"), Maps[0].MapId, FString(TEXT("arena")));
				TestEqual(TEXT("world min x"), Maps[0].WorldMinX, -100.0);
			}
		});

		It("parses a maps response with null Z bounds", [this]()
		{
			const FString Json = TEXT(R"JSON({"success":true,"data":[{"id":"11111111-1111-1111-1111-111111111111","name":"Arena","mapId":"arena","imageUrl":"https://example.test/map.png","worldMinX":-100,"worldMinY":-50,"worldMaxX":100,"worldMaxY":75,"worldMinZ":null,"worldMaxZ":null,"imageWidth":1024,"imageHeight":512,"createdAt":"2026-07-01T00:00:00Z","updatedAt":"2026-07-02T00:00:00Z"}]})JSON");
			TArray<FramedashEditor::FMapInfo> Maps;
			FString Error;

			TestTrue(TEXT("maps response with null Z bounds accepted"), FramedashEditor::ParseMapsResponse(Json, Maps, Error));
			TestEqual(TEXT("one map parsed"), Maps.Num(), 1);
			if (Maps.Num() == 1)
			{
				TestFalse(TEXT("null world min Z is unset"), Maps[0].WorldMinZ.IsSet());
				TestFalse(TEXT("null world max Z is unset"), Maps[0].WorldMaxZ.IsSet());
			}
		});

		It("parses a valid heatmap response with nullable metrics", [this]()
		{
			const FString Json = TEXT(R"JSON({"success":true,"data":[{"x":12.5,"y":37.5,"z":62.5,"weight":8,"event_count":8,"avg_fps":59.5,"avg_frame_time":16.8,"avg_memory":2048,"avg_gpu_time":null,"avg_mem_vram":null}]})JSON");
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FString Error;

			TestTrue(TEXT("valid heatmap response accepted"), FramedashEditor::ParseHeatmapResponse(Json, Cells, Error));
			TestEqual(TEXT("one cell parsed"), Cells.Num(), 1);
			if (Cells.Num() == 1)
			{
				TestEqual(TEXT("cell x"), Cells[0].X, 12.5);
				TestEqual(TEXT("cell z"), Cells[0].Z.Get(0.0), 62.5);
				TestFalse(TEXT("null gpu time is unset"), Cells[0].AverageGpuTime.IsSet());
				TestFalse(TEXT("null vram is unset"), Cells[0].AverageVramMemory.IsSet());
			}
		});

		It("parses ClickHouse UInt64 strings in a heatmap response", [this]()
		{
			const FString Json = TEXT(R"JSON({"success":true,"data":[{"x":12.5,"y":37.5,"weight":"8","event_count":"8","avg_fps":59.5,"avg_frame_time":16.8,"avg_memory":2048,"avg_gpu_time":null,"avg_mem_vram":null}]})JSON");
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FString Error;

			TestTrue(TEXT("quoted UInt64 fields accepted"), FramedashEditor::ParseHeatmapResponse(Json, Cells, Error));
			TestEqual(TEXT("one cell parsed"), Cells.Num(), 1);
			if (Cells.Num() == 1)
			{
				TestEqual(TEXT("quoted weight parsed"), Cells[0].Weight, 8.0);
				TestEqual(TEXT("quoted event count parsed"), Cells[0].EventCount, 8.0);
			}
		});

		It("extracts RFC 9457 detail then falls back to title", [this]()
		{
			const FString Detailed = TEXT(R"JSON({"title":"Forbidden","status":403,"detail":"analytics:read scope required","error_category":"authorization","retryable":false})JSON");
			const FString Titled = TEXT(R"JSON({"title":"Too Many Requests","status":429,"detail":"","error_category":"rate_limit","retryable":true})JSON");
			TestEqual(TEXT("detail preferred"), FramedashEditor::ParseProblemMessage(Detailed, TEXT("fallback")), FString(TEXT("analytics:read scope required")));
			TestEqual(TEXT("title fallback"), FramedashEditor::ParseProblemMessage(Titled, TEXT("fallback")), FString(TEXT("Too Many Requests")));
		});
	});

	Describe("heatmap geometry", [this]()
	{
		It("keeps the viewport show flag off by default and independent per viewport", [this]()
		{
			FEngineShowFlags FirstViewport(ESFIM_Editor);
			FEngineShowFlags SecondViewport(ESFIM_Editor);

			TestFalse(
				TEXT("first viewport starts hidden"),
				FramedashEditor::IsHeatmapShowFlagEnabled(FirstViewport));
			TestFalse(
				TEXT("second viewport starts hidden"),
				FramedashEditor::IsHeatmapShowFlagEnabled(SecondViewport));

			TestTrue(
				TEXT("show flag can be enabled"),
				FramedashEditor::SetHeatmapShowFlagEnabled(FirstViewport, true));
			TestTrue(
				TEXT("first viewport is enabled"),
				FramedashEditor::IsHeatmapShowFlagEnabled(FirstViewport));
			TestFalse(
				TEXT("second viewport remains independent"),
				FramedashEditor::IsHeatmapShowFlagEnabled(SecondViewport));
		});

		It("suspends rendering for PIE without changing the viewport choice", [this]()
		{
			FEngineShowFlags ViewportFlags(ESFIM_Editor);
			FramedashEditor::SetHeatmapShowFlagEnabled(ViewportFlags, true);

			TestTrue(
				TEXT("editor view renders before PIE"),
				FramedashEditor::ShouldRenderHeatmap(ViewportFlags, false));
			TestFalse(
				TEXT("PIE suspends the overlay"),
				FramedashEditor::ShouldRenderHeatmap(ViewportFlags, true));
			TestTrue(
				TEXT("the prior editor choice restores after PIE"),
				FramedashEditor::ShouldRenderHeatmap(ViewportFlags, false));
		});

		It("suspends a scene component created after PIE begins", [this]()
		{
			FramedashEditor::FMapInfo Map;
			Map.WorldMaxX = 100.0;
			Map.WorldMaxY = 100.0;
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FramedashEditor::FHeatmapCell& Cell = Cells.AddDefaulted_GetRef();
			Cell.X = 12.5;
			Cell.Y = 12.5;

			FFramedashEditorHeatmapOverlay Overlay;
			FEditorDelegates::BeginPIE.Broadcast(false);
			Overlay.SetData(Map, Cells, 25.0);
			UFramedashEditorHeatmapComponent* Component =
				Overlay.GetRenderComponentForTesting();
			TestNotNull(TEXT("in-flight response creates a scene component"), Component);
			if (Component != nullptr)
			{
				TestTrue(
					TEXT("component created during PIE stays suspended"),
					Component->IsPIESuspended());
			}
			FEditorDelegates::EndPIE.Broadcast(false);
		});

		It("keeps standard and high-resolution screenshots on the scene main pass", [this]()
		{
			FPrimitiveViewRelevance Relevance;
			FramedashEditor::ConfigureHeatmapViewRelevance(Relevance, true);

			TestTrue(TEXT("visible heatmap draws"), Relevance.bDrawRelevance);
			TestTrue(TEXT("heatmap uses dynamic scene geometry"), Relevance.bDynamicRelevance);
			TestTrue(TEXT("F9 and high-resolution capture include the main pass"),
				Relevance.bRenderInMainPass);
		});

		It("reconstructs an interior cell", [this]()
		{
			FramedashEditor::FMapInfo Map;
			Map.WorldMinX = -100.0;
			Map.WorldMinY = -100.0;
			Map.WorldMaxX = 100.0;
			Map.WorldMaxY = 100.0;
			FramedashEditor::FHeatmapCell Cell;
			Cell.X = -62.5;
			Cell.Y = 12.5;

			const FramedashEditor::FCellRect Rect = FramedashEditor::BuildCellRect(Cell, Map, 25.0);
			TestEqual(TEXT("min x"), Rect.MinX, -75.0);
			TestEqual(TEXT("min y"), Rect.MinY, 0.0);
			TestEqual(TEXT("max x"), Rect.MaxX, -50.0);
			TestEqual(TEXT("max y"), Rect.MaxY, 25.0);
		});

		It("clamps an edge cell to map maximums", [this]()
		{
			FramedashEditor::FMapInfo Map;
			Map.WorldMinX = 0.0;
			Map.WorldMinY = 0.0;
			Map.WorldMaxX = 93.0;
			Map.WorldMaxY = 87.0;
			FramedashEditor::FHeatmapCell Cell;
			Cell.X = 90.0;
			Cell.Y = 85.0;

			const FramedashEditor::FCellRect Rect = FramedashEditor::BuildCellRect(Cell, Map, 25.0);
			TestEqual(TEXT("edge min x"), Rect.MinX, 75.0);
			TestEqual(TEXT("edge min y"), Rect.MinY, 75.0);
			TestEqual(TEXT("edge max x clamped"), Rect.MaxX, 93.0);
			TestEqual(TEXT("edge max y clamped"), Rect.MaxY, 87.0);
		});

		It("applies an explicit editor-world alignment offset", [this]()
		{
			FramedashEditor::FMapInfo Map;
			Map.WorldMinX = -100.0;
			Map.WorldMinY = -100.0;
			Map.WorldMaxX = 100.0;
			Map.WorldMaxY = 100.0;
			FramedashEditor::FHeatmapCell Cell;
			Cell.X = 12.5;
			Cell.Y = 37.5;

			const FramedashEditor::FCellRect Rect = FramedashEditor::BuildCellRect(
				Cell,
				Map,
				25.0,
				FVector2D(-1500.0, -1700.0));
			TestEqual(TEXT("aligned min x"), Rect.MinX, -1500.0);
			TestEqual(TEXT("aligned min y"), Rect.MinY, -1675.0);
			TestEqual(TEXT("aligned max x"), Rect.MaxX, -1475.0);
			TestEqual(TEXT("aligned max y"), Rect.MaxY, -1650.0);
		});

		It("normalizes equal weights without division by zero", [this]()
		{
			TArray<FramedashEditor::FHeatmapCell> Cells;
			Cells.SetNum(3);
			for (auto&& Cell : Cells)
			{
				Cell.Weight = 0.0;
			}
			const double MaxWeight = FramedashEditor::FindMaxWeight(Cells);
			TestEqual(TEXT("equal zero max"), MaxWeight, 0.0);
			TestEqual(TEXT("zero batch normalization"), FramedashEditor::NormalizeWeight(0.0, MaxWeight), 0.0);
		});

		It("builds a voxel around the measured Z bin", [this]()
		{
			const FramedashEditor::FCellRect Rect{0.0, 25.0, 25.0, 50.0};
			FramedashEditor::FHeatmapCell Cell;
			Cell.Z = 62.5;
			const TStaticArray<FVector, 8> Corners =
				FramedashEditor::BuildHeatmapCellCorners(Rect, Cell, 0.0, 25.0);

			TestTrue(TEXT("voxel minimum uses measured bin"),
				Corners[0].Equals(FVector(1.25, 26.25, 51.25)));
			TestTrue(TEXT("voxel maximum uses measured bin"),
				Corners[6].Equals(FVector(23.75, 48.75, 73.75)));
		});

		It("keeps cells without measured Z flat at the map floor", [this]()
		{
			const FramedashEditor::FCellRect Rect{0.0, 25.0, 25.0, 50.0};
			const FramedashEditor::FHeatmapCell Cell;
			const TStaticArray<FVector, 8> Corners =
				FramedashEditor::BuildHeatmapCellCorners(Rect, Cell, 100.0, 25.0);

			TestEqual(TEXT("flat fallback minimum Z"), Corners[0].Z, 100.0);
			TestEqual(TEXT("flat fallback maximum Z"), Corners[4].Z, 100.0);
		});

		It("distinguishes measured voxels from flat fallback cells", [this]()
		{
			FramedashEditor::FHeatmapCell MeasuredCell;
			MeasuredCell.Z = 62.5;
			const FramedashEditor::FHeatmapCell FlatCell;

			TestTrue(TEXT("measured cell is volumetric"),
				FramedashEditor::IsVolumetricHeatmapCell(MeasuredCell, 25.0));
			TestFalse(TEXT("cell without Z is flat"),
				FramedashEditor::IsVolumetricHeatmapCell(FlatCell, 25.0));
		});

		It("uses a five-stop promotional heatmap palette", [this]()
		{
			const FLinearColor Cold = FramedashEditor::HeatmapColor(0.0, 0.6f);
			const FLinearColor Cool = FramedashEditor::HeatmapColor(0.25, 0.6f);
			const FLinearColor Mid = FramedashEditor::HeatmapColor(0.5, 0.6f);
			const FLinearColor Warm = FramedashEditor::HeatmapColor(0.75, 0.6f);
			const FLinearColor Hot = FramedashEditor::HeatmapColor(1.0, 0.6f);

			TestTrue(TEXT("cold is blue"), Cold.Equals(FLinearColor(0.0f, 0.1f, 1.0f, 0.6f)));
			TestTrue(TEXT("cool is cyan"), Cool.Equals(FLinearColor(0.0f, 1.0f, 1.0f, 0.6f)));
			TestTrue(TEXT("mid is green"), Mid.Equals(FLinearColor(0.0f, 1.0f, 0.2f, 0.6f)));
			TestTrue(TEXT("warm is yellow"), Warm.Equals(FLinearColor(1.0f, 1.0f, 0.0f, 0.6f)));
			TestTrue(TEXT("hot is red"), Hot.Equals(FLinearColor(1.0f, 0.05f, 0.0f, 0.6f)));
		});

		It("batches representative multi-height cells without changing cell or color semantics", [this]()
		{
			TArray<FramedashEditor::FHeatmapSceneCell> SceneCells;
			const TStaticArray<double, 3> Weights = {1.0, 0.0, 0.5};
			for (int32 CellIndex = 0; CellIndex < 3; ++CellIndex)
			{
				FramedashEditor::FHeatmapSceneCell& SceneCell =
					SceneCells.AddDefaulted_GetRef();
				const double CenterZ = 25.0 + CellIndex * 50.0;
				SceneCell.WorldCorners = FramedashEditor::BuildVoxelCorners(
					FramedashEditor::FCellRect{
						static_cast<double>(CellIndex * 25),
						0.0,
						static_cast<double>((CellIndex + 1) * 25),
						25.0},
					CenterZ,
					25.0);
				SceneCell.NormalizedWeight = Weights[CellIndex];
				SceneCell.bVolumetric = true;
			}

			const FramedashEditor::FHeatmapRenderData RenderData =
				FramedashEditor::BuildHeatmapRenderData(SceneCells, 0.6f, 10.0);

			TestEqual(TEXT("all cloud cells remain represented"), RenderData.CellCount, 3);
			TestEqual(TEXT("three voxels keep twelve triangles each"), RenderData.TriangleCount, 36);
			TestEqual(TEXT("three distinct weights retain three colors"), RenderData.Buckets.Num(), 3);
			TestTrue(TEXT("low weight draws first and remains blue"),
				RenderData.Buckets[0].Color.Equals(
					FramedashEditor::HeatmapColor(0.0, 0.6f)));
			TestTrue(TEXT("high weight draws last and remains red"),
				RenderData.Buckets.Last().Color.Equals(
					FramedashEditor::HeatmapColor(1.0, 0.6f)));
			TestEqual(
				TEXT("display Z offset applies to scene geometry"),
				static_cast<double>(RenderData.Bounds.Min.Z),
				23.75);
			TestEqual(
				TEXT("multi-height maximum remains distinct"),
				static_cast<double>(RenderData.Bounds.Max.Z),
				146.25);
		});

		It("adds compact proportional padding for viewport framing", [this]()
		{
			const FBox CellBounds(FVector(0.0, 0.0, 100.0), FVector(50.0, 50.0, 100.0));
			const FBox FramingBounds = FramedashEditor::BuildHeatmapFramingBounds(CellBounds);

			TestTrue(TEXT("framing bounds remain valid"), FramingBounds.IsValid != 0);
			TestTrue(TEXT("horizontal padding stays compact"),
				FramingBounds.Min.Equals(FVector(-25.0, -25.0, 75.0)));
			TestTrue(TEXT("flat heatmaps retain usable depth"),
				FramingBounds.Max.Equals(FVector(75.0, 75.0, 125.0)));
		});

		It("includes the display Z offset in viewport framing", [this]()
		{
			const FBox CellBounds(FVector(0.0, 0.0, 100.0), FVector(50.0, 50.0, 100.0));
			const FBox FramingBounds = FramedashEditor::BuildHeatmapFramingBounds(CellBounds, 40.0);

			TestTrue(TEXT("offset framing minimum follows rendered geometry"),
				FramingBounds.Min.Equals(FVector(-25.0, -25.0, 115.0)));
			TestTrue(TEXT("offset framing maximum follows rendered geometry"),
				FramingBounds.Max.Equals(FVector(75.0, 75.0, 165.0)));
		});

		It("invalidates cached framing bounds when data is cleared", [this]()
		{
			FramedashEditor::FMapInfo Map;
			Map.WorldMaxX = 100.0;
			Map.WorldMaxY = 100.0;
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FramedashEditor::FHeatmapCell& Cell = Cells.AddDefaulted_GetRef();
			Cell.X = 12.5;
			Cell.Y = 12.5;
			Cell.Z = 62.5;

			FFramedashEditorHeatmapOverlay Overlay;
			Overlay.SetData(Map, Cells, 25.0);
			FBox Bounds;
			TestTrue(
				TEXT("loaded data exposes framing bounds"),
				Overlay.GetWorldBounds(Bounds));
			Overlay.ClearData();
			TestFalse(
				TEXT("cleared data has no framing bounds"),
				Overlay.GetWorldBounds(Bounds));
		});

		It("releases the registered scene component when data is cleared", [this]()
		{
			FramedashEditor::FMapInfo Map;
			Map.WorldMaxX = 100.0;
			Map.WorldMaxY = 100.0;
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FramedashEditor::FHeatmapCell& Cell = Cells.AddDefaulted_GetRef();
			Cell.X = 12.5;
			Cell.Y = 12.5;

			FFramedashEditorHeatmapOverlay Overlay;
			Overlay.SetData(Map, Cells, 25.0);
			UFramedashEditorHeatmapComponent* Component =
				Overlay.GetRenderComponentForTesting();
			TestNotNull(TEXT("loaded heatmap owns a scene component"), Component);
			if (Component == nullptr)
			{
				return;
			}
			TestTrue(TEXT("loaded scene component is registered"), Component->IsRegistered());
			const uint64 LoadedDataRevision = Overlay.GetDataRevision();

			Overlay.ClearData();

			TestFalse(TEXT("cleared scene component is unregistered"), Component->IsRegistered());
			TestFalse(
				TEXT("clear invalidates in-flight response generations"),
				Overlay.IsDataRevisionCurrent(LoadedDataRevision));
			TestNull(
				TEXT("cleared overlay releases the scene component"),
				Overlay.GetRenderComponentForTesting());
		});

		It("invalidates loaded data when query settings change without a panel", [this]()
		{
			UFramedashEditorSettings* Settings =
				GetMutableDefault<UFramedashEditorSettings>();
			TestNotNull(TEXT("editor settings are available"), Settings);
			if (Settings == nullptr)
			{
				return;
			}

			FramedashEditor::FMapInfo Map;
			Map.WorldMaxX = 100.0;
			Map.WorldMaxY = 100.0;
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FramedashEditor::FHeatmapCell& Cell = Cells.AddDefaulted_GetRef();
			Cell.X = 12.5;
			Cell.Y = 12.5;

			FFramedashEditorHeatmapOverlay Overlay;
			Overlay.SetData(Map, Cells, 25.0);
			FBox Bounds;
			TestTrue(TEXT("loaded data starts valid"), Overlay.GetWorldBounds(Bounds));

			const int32 OriginalDays = Settings->Days;
			Settings->Days = OriginalDays == 30 ? 14 : 30;
			FProperty* DaysProperty = FindFProperty<FProperty>(
				UFramedashEditorSettings::StaticClass(),
				GET_MEMBER_NAME_CHECKED(UFramedashEditorSettings, Days));
			FPropertyChangedEvent DaysChangedEvent(
				DaysProperty,
				EPropertyChangeType::ValueSet);
			FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(
				Settings,
				DaysChangedEvent);

			TestFalse(
				TEXT("query change clears module-owned overlay data"),
				Overlay.GetWorldBounds(Bounds));

			Settings->Days = OriginalDays;
			FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(
				Settings,
				DaysChangedEvent);
		});
	});

	Describe("endpoint security", [this]()
	{
		It("matches the runtime endpoint allowlist", [this]()
		{
			TestTrue(TEXT("https allowed"), FramedashEditor::IsEndpointSecure("https://app.framedash.dev"));
			TestTrue(TEXT("localhost http allowed"), FramedashEditor::IsEndpointSecure("http://localhost:3000"));
			TestFalse(TEXT("arbitrary http rejected"), FramedashEditor::IsEndpointSecure("http://example.com"));
			TestFalse(TEXT("userinfo host confusion rejected"), FramedashEditor::IsEndpointSecure("http://localhost@evil.example"));
		});

		It("detects cross-origin redirects", [this]()
		{
			TestFalse(TEXT("identical URL stays same origin"), FramedashEditor::IsCrossOriginRedirect(
				"https://app.framedash.dev/api/v1/maps",
				"https://app.framedash.dev/api/v1/maps"));
			TestTrue(TEXT("different host is cross origin"), FramedashEditor::IsCrossOriginRedirect(
				"https://app.framedash.dev/api/v1/maps",
				"https://attacker.example/api/v1/maps"));
			TestTrue(TEXT("malformed port fails closed"), FramedashEditor::IsCrossOriginRedirect(
				"https://app.framedash.dev/api/v1/maps",
				"https://app.framedash.dev:abc/api/v1/maps"));
			TestTrue(TEXT("multi-colon port fails closed"), FramedashEditor::IsCrossOriginRedirect(
				"https://app.framedash.dev/api/v1/maps",
				"https://app.framedash.dev:443:evil/api/v1/maps"));
			TestFalse(TEXT("missing effective URL fails open"), FramedashEditor::IsCrossOriginRedirect(
				"https://app.framedash.dev/api/v1/maps",
				""));
			// Divergence from the runtime module's IsCrossOriginRedirect (which fails
			// open on an unparseable origin to avoid dropping telemetry): this editor
			// read path has no telemetry-loss cost, so an unparseable effective URL
			// (unsupported scheme here) must fail CLOSED instead.
			TestTrue(TEXT("unparseable effective origin fails closed"), FramedashEditor::IsCrossOriginRedirect(
				"https://app.framedash.dev/api/v1/maps",
				"ftp://app.framedash.dev/api/v1/maps"));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_EDITOR_WITH_AUTOMATION_SPECS
