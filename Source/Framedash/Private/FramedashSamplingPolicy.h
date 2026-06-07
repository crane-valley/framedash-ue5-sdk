// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"

/**
 * Random sampling policy with a global default rate plus per-event-name overrides.
 * If an override is set for a given event name it takes precedence; otherwise the
 * global rate applies.
 */
class FFramedashSamplingPolicy
{
public:
	explicit FFramedashSamplingPolicy(float InRate = 1.0f);

	/** Returns true if the event should be recorded, using the per-event override when present. */
	bool ShouldSample(const FString& EventName) const;

	void SetRate(float InRate);
	float GetRate() const;

	/** Set a per-event-name override rate. Empty names are ignored. Rate is clamped to [0, 1]. */
	void SetEventRate(const FString& EventName, float InRate);

	/** Remove a per-event-name override. Returns true if an override was present. */
	bool RemoveEventRate(const FString& EventName);

private:
	float Rate;
	mutable FCriticalSection EventRatesLock;
	TMap<FString, float> EventRates;
};
