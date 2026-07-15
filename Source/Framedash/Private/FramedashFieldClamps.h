// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Engine-independent client-side field clamps that mirror the ingest server's
// per-event validation limits (packages/ingest-core/src/config.ts +
// validation.ts). Ingest validation rejects the ENTIRE batch if any single
// event field violates a limit, AFTER the server already returned 202, so one
// over-limit field silently drops every event in that flush. Clamping each
// field client-side keeps a single bad physics frame (NaN position, Inf metric,
// sub-1ms frame time) from losing unrelated telemetry in the same flush.
//
// This header is pure C++ on purpose: it includes NO Unreal headers (no
// CoreMinimal.h / FString / FMath), so the engine-independent GoogleTest harness
// can include and unit-test it without an UnrealEditor build. String truncation
// is intentionally NOT here -- FString truncation uses TruncateString (FString::
// Left, UTF-16 code-unit count) which matches the server's JS string-length
// semantics; a byte-based std::string truncation would diverge from the server.
//
// Numeric behavior replicates the Godot SDK (sdks/godot/Runtime) exactly.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Framedash
{
namespace FieldClamps
{
	// Server limits mirrored here (packages/ingest-core/src/config.ts). Kept as
	// pure-C++ constants so this header has no dependency on the Unreal-side
	// FramedashConstants (which pulls in CoreMinimal via FramedashTypes.h).
	constexpr double PositionAbsMax = 1e9;   // POSITION_ABS_MAX
	constexpr float FpsMax = 1000.0f;        // FPS_MAX
	constexpr float TimingMsMax = 10000.0f;  // FRAME/GPU/GAME/RENDER thread ms ceiling

	// position x/y/z: ingest rejects NaN/Inf or |v| > 1e9 (validation.ts
	// POSITION_FIELDS). Map non-finite to 0 and clamp magnitude to the cap.
	// Mirrors Godot TelemetrySDK.SanitizeCoord (double here -- UE5 FVector is
	// double-precision).
	inline double SanitizeCoord(double v)
	{
		if (!std::isfinite(v)) return 0.0;
		if (v > PositionAbsMax) return PositionAbsMax;
		if (v < -PositionAbsMax) return -PositionAbsMax;
		return v;
	}

	// frame_time_ms / gpu_time_ms / game_thread_ms / render_thread_ms: ingest
	// rejects NaN/Inf or values outside [0, 10000] (validation.ts
	// NUMERIC_RANGE_RULES). Map non-finite/negative to 0 (the proto contract
	// treats 0 as "not collected") and cap at the ceiling. Mirrors the Godot
	// PerformanceCollector per-metric guard ((NaN || <0) ? 0 : min(v, 10000)).
	inline float ClampTimingMs(float v)
	{
		if (!std::isfinite(v) || v < 0.0f) return 0.0f;
		return std::min(v, TimingMsMax);
	}

	// fps: ingest rejects NaN/Inf or values outside [0, 1000] (validation.ts
	// NUMERIC_RANGE_RULES). The UE PerformanceCollector computes fps as
	// 1.0/DeltaTime, so clamp the computed value directly. Mirrors the Godot
	// Collect() ceiling clamp ((NaN || <0) ? 0 : min(fps, 1000)).
	inline float ClampFps(float fps)
	{
		if (!std::isfinite(fps) || fps < 0.0f) return 0.0f;
		return std::min(fps, FpsMax);
	}

	// Replicate Godot's fps-from-frame-time path exactly: fps = frameTimeMs > 0 ?
	// min(1000, 1000 / frameTimeMs) : 0. Provided for parity with the Godot
	// canonical reference; the UE collector uses ClampFps on its own 1/DeltaTime
	// computation (it tracks frame time separately).
	inline float FpsFromFrameTimeMs(float frameTimeMs)
	{
		if (!std::isfinite(frameTimeMs) || frameTimeMs <= 0.0f) return 0.0f;
		return std::min(FpsMax, 1000.0f / frameTimeMs);
	}

	// memory_used_bytes: ingest rejects values outside [0, 64 GiB] (validation.ts
	// MEMORY_USED_BYTES_MAX). Floor a negative/garbage reading at 0 and cap an
	// oversized one (high-memory editor / workstation / dedicated-server runs) at
	// the ceiling, so one large sample cannot drop the whole batch.
	constexpr int64_t MemoryBytesMax = 64LL * 1024 * 1024 * 1024; // 64 GiB
	inline int64_t ClampMemoryBytes(int64_t v)
	{
		if (v < 0) return 0;
		return v < MemoryBytesMax ? v : MemoryBytesMax;
	}
} // namespace FieldClamps
} // namespace Framedash
