// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"
#include "FramedashMemorySample.h"

class FFramedashPerformanceCollector
{
public:
	FFramedashPerformanceSnapshot Collect() const;

	// Sample memory-category detail (RHI VRAM + optional LLM breakdown) for the
	// mem.* heartbeat metrics. Returns raw inputs; the engine-independent
	// SelectMemoryMetrics() decides which keys are present. Thin engine-API layer
	// on purpose so the emit rules stay host-testable. Game-thread only; reads
	// cached RHI/LLM counters (no per-frame work). Fail-safe: an unavailable
	// source yields "absent" (bRhiValid / bLlmEnabled false), never a crash.
	Framedash::FMemorySampleInputs SampleMemoryDetail() const;
};
