// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashPerformanceCollector.h"
#include "FramedashFieldClamps.h"
#include "DynamicRHI.h"
#include "GenericPlatform/GenericPlatformMemory.h"
#include "HAL/LowLevelMemTracker.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RenderCore.h"
#include "RHIGlobals.h"
#include "RHIStats.h"

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

Framedash::FMemorySampleInputs FFramedashPerformanceCollector::SampleMemoryDetail() const
{
	Framedash::FMemorySampleInputs In;

	// mem.vram via RHI texture memory stats. The RHIGetTextureMemoryStats free
	// function dereferences GDynamicRHI unconditionally, so guard on it first:
	// GDynamicRHI is null during a cook commandlet and before RHI init -- calling
	// blind would crash (fail-safe: leave the key absent instead). This differs
	// from RHIGetGPUFrameCycles() above, which null-checks internally and returns 0.
	// Also skip when GUsingNullRHI: under -nullrhi (our own CI runs it, plus
	// dedicated servers) GDynamicRHI points to a real FNullDynamicRHI object (so the
	// null check alone passes) whose stats are all zero -- emitting mem.vram=0 there
	// would report a fabricated 0 for an environment that has no GPU memory, so
	// leave the key absent (absent = not collected).
	if (GDynamicRHI != nullptr && !GUsingNullRHI)
	{
		FTextureMemoryStats TexStats;
		RHIGetTextureMemoryStats(TexStats);
		// "Video memory in use" = actually-allocated texture memory: the sum of
		// streaming + non-streaming texture allocations (both uint64, default 0).
		// Why not the other fields: DedicatedVideoMemory / TotalGraphicsMemory are
		// hardware CAPACITY (not usage), and TexturePoolSize is a configured budget
		// (0 when the streaming pool limit is disabled). The streaming +
		// non-streaming sum is the closest RHI-provided proxy for live VRAM
		// footprint and, unlike LLM, is available on every RHI without -llm. Reads
		// cached counters, so it is cheap enough at the 10s heartbeat cadence.
		In.bRhiValid = true;
		In.VramBytes = static_cast<int64>(TexStats.StreamingMemorySize + TexStats.NonStreamingMemorySize);
	}

#if ENABLE_LOW_LEVEL_MEM_TRACKER
	// LLM category breakdown: gated by BOTH this compile-time #if (LLM compiled in)
	// AND IsEnabled() (LLM actually activated via -llm at runtime). GetTagAmount-
	// ForTracker returns the current tracked bytes for the ELLMTag on the Default
	// tracker (0 for an untracked/empty tag); SelectMemoryMetrics drops non-positive
	// amounts so an unpopulated category stays absent. IsEnabled() can read true
	// during very early startup before the commandline is parsed, but the heartbeat
	// first fires ~10s in, long after ProcessCommandLine, so it reflects the real
	// -llm state here.
	if (FLowLevelMemTracker::IsEnabled())
	{
		FLowLevelMemTracker& Llm = FLowLevelMemTracker::Get();
		In.bLlmEnabled = true;
		In.TexturesBytes = Llm.GetTagAmountForTracker(ELLMTracker::Default, ELLMTag::Textures);
		In.MeshesBytes = Llm.GetTagAmountForTracker(ELLMTracker::Default, ELLMTag::Meshes);
		In.AudioBytes = Llm.GetTagAmountForTracker(ELLMTracker::Default, ELLMTag::Audio);
	}
#endif

	return In;
}
