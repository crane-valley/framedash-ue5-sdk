// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashSamplingPolicy.h"

FFramedashSamplingPolicy::FFramedashSamplingPolicy(float InRate)
	: Rate(FMath::Clamp(InRate, 0.0f, 1.0f))
{
}

bool FFramedashSamplingPolicy::ShouldSample(const FString& EventName) const
{
	float EffectiveRate;
	{
		FScopeLock Lock(&EventRatesLock);
		EffectiveRate = Rate;
		if (!EventName.IsEmpty())
		{
			if (const float* Override = EventRates.Find(EventName))
			{
				EffectiveRate = *Override;
			}
		}
	}

	if (EffectiveRate >= 1.0f) return true;
	if (EffectiveRate <= 0.0f) return false;
	return FMath::FRand() <= EffectiveRate;
}

void FFramedashSamplingPolicy::SetRate(float InRate)
{
	const float Clamped = FMath::Clamp(InRate, 0.0f, 1.0f);
	FScopeLock Lock(&EventRatesLock);
	Rate = Clamped;
}

float FFramedashSamplingPolicy::GetRate() const
{
	FScopeLock Lock(&EventRatesLock);
	return Rate;
}

void FFramedashSamplingPolicy::SetEventRate(const FString& EventName, float InRate)
{
	if (EventName.IsEmpty()) return;
	const float Clamped = FMath::Clamp(InRate, 0.0f, 1.0f);
	FScopeLock Lock(&EventRatesLock);
	EventRates.Add(EventName, Clamped);
}

bool FFramedashSamplingPolicy::RemoveEventRate(const FString& EventName)
{
	if (EventName.IsEmpty()) return false;
	FScopeLock Lock(&EventRatesLock);
	return EventRates.Remove(EventName) > 0;
}
