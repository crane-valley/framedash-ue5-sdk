// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"

/** Collects performance metrics (FPS, frame time, memory). */
class FFramedashPerformanceCollector
{
public:
	/** Collect a snapshot of current performance metrics. */
	FFramedashPerformanceSnapshot Collect() const;
};
