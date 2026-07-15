// Copyright Crane Valley. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_EDITOR_WITH_AUTOMATION_SPECS

#include "Misc/AutomationTest.h"

#include "FramedashEditorEndpointSecurity.h"
#include "FramedashEditorLogic.h"

BEGIN_DEFINE_SPEC(FFramedashEditorHeatmapSpec, "Framedash.Editor.Heatmap.Logic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FFramedashEditorHeatmapSpec)

void FFramedashEditorHeatmapSpec::Define()
{
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
			const FString Json = TEXT(R"JSON({"success":true,"data":[{"x":12.5,"y":37.5,"weight":8,"event_count":8,"avg_fps":59.5,"avg_frame_time":16.8,"avg_memory":2048,"avg_gpu_time":null,"avg_mem_vram":null}]})JSON");
			TArray<FramedashEditor::FHeatmapCell> Cells;
			FString Error;

			TestTrue(TEXT("valid heatmap response accepted"), FramedashEditor::ParseHeatmapResponse(Json, Cells, Error));
			TestEqual(TEXT("one cell parsed"), Cells.Num(), 1);
			if (Cells.Num() == 1)
			{
				TestEqual(TEXT("cell x"), Cells[0].X, 12.5);
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

		It("normalizes equal weights without division by zero", [this]()
		{
			TArray<FramedashEditor::FHeatmapCell> Cells;
			Cells.SetNum(3);
			for (FramedashEditor::FHeatmapCell& Cell : Cells)
			{
				Cell.Weight = 0.0;
			}
			const double MaxWeight = FramedashEditor::FindMaxWeight(Cells);
			TestEqual(TEXT("equal zero max"), MaxWeight, 0.0);
			TestEqual(TEXT("zero batch normalization"), FramedashEditor::NormalizeWeight(0.0, MaxWeight), 0.0);
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
