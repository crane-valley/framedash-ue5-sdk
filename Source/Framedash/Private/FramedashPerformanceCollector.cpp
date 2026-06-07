// Copyright Crane Valley. All Rights Reserved.

#include "FramedashPerformanceCollector.h"
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
	FFramedashPerformanceSnapshot Snapshot;

	const double DeltaTime = FApp::GetDeltaTime();
	if (DeltaTime > SMALL_NUMBER)
	{
		Snapshot.Fps = static_cast<float>(1.0 / DeltaTime);
		Snapshot.FrameTimeMs = static_cast<float>(DeltaTime * 1000.0);
	}

	FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	Snapshot.MemoryUsedBytes = static_cast<int64>(MemStats.UsedPhysical);

	// GPU frame time via RHI (returns CPU cycles — convert to ms).
	// Returns 0 when GPU profiling is unavailable (headless server, etc.).
	Snapshot.GpuTimeMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());

	// Thread timing: globals are in CPU cycles — convert to milliseconds.
	// Clamp to 0 to prevent negative/garbage values (e.g. before first frame)
	// from failing server-side validation and dropping the entire event.
	Snapshot.GameThreadMs = FMath::Max(0.0f, FPlatformTime::ToMilliseconds(GGameThreadTime));
	Snapshot.RenderThreadMs = FMath::Max(0.0f, FPlatformTime::ToMilliseconds(GRenderThreadTime));

	return Snapshot;
}
