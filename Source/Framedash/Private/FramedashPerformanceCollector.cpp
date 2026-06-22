// Copyright Crane Valley. All Rights Reserved.

#include "FramedashPerformanceCollector.h"
#include "FramedashFieldClamps.h"
#include "DynamicRHI.h"
#include "GenericPlatform/GenericPlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RenderCore.h"

// Engine globals updated each frame — declared in RenderCore module.
// Values are in CPU cycles (uint32). Convert via FPlatformTime::ToMilliseconds().
// NOTE: Previous code incorrectly declared these as float — the actual type is uint32
// (see RenderTimer.h). Using the wrong type causes bit-reinterpretation (UB).
extern RENDERCORE_API uint32 GGameThreadTime;
extern RENDERCORE_API uint32 GRenderThreadTime;

FFramedashPerformanceSnapshot FFramedashPerformanceCollector::Collect() const
{
	using namespace Framedash::FieldClamps;

	FFramedashPerformanceSnapshot Snapshot;

	const double DeltaTime = FApp::GetDeltaTime();
	if (DeltaTime > SMALL_NUMBER)
	{
		// Clamp both to the ingest ranges (fps [0,1000], frame_time [0,10000]):
		// an uncapped sub-1ms frame would report fps > 1000, and a long
		// pause/resume gap could exceed the frame-time ceiling -- either makes
		// the validator drop the whole batch.
		Snapshot.Fps = ClampFps(static_cast<float>(1.0 / DeltaTime));
		Snapshot.FrameTimeMs = ClampTimingMs(static_cast<float>(DeltaTime * 1000.0));
	}

	FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	// UsedPhysical is unsigned; the cast can produce a negative int64 only on an
	// implausibly huge reading, but clamp defensively (ingest rejects < 0).
	Snapshot.MemoryUsedBytes = ClampMemoryBytes(static_cast<int64>(MemStats.UsedPhysical));

	// GPU frame time via RHI (returns CPU cycles — convert to ms).
	// Returns 0 when GPU profiling is unavailable (headless server, etc.).
	// Clamp to the ingest range so a NaN/negative/over-ceiling reading cannot
	// drop the batch.
	Snapshot.GpuTimeMs = ClampTimingMs(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));

	// Thread timing: globals are in CPU cycles — convert to milliseconds, then
	// clamp to the ingest range (replaces the prior >=0 floor; also caps the
	// ceiling and rejects non-finite values).
	Snapshot.GameThreadMs = ClampTimingMs(FPlatformTime::ToMilliseconds(GGameThreadTime));
	Snapshot.RenderThreadMs = ClampTimingMs(FPlatformTime::ToMilliseconds(GRenderThreadTime));

	return Snapshot;
}
