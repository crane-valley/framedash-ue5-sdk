// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include <cmath>

namespace Framedash
{
	// Takes double so FRotator components (double under UE5 Large World
	// Coordinates) pass without an implicit narrowing conversion at the call site;
	// the final narrowing to the float wire type is an explicit cast here.

	// Normalize a yaw angle (degrees) to [0, 360). Input may be any finite value.
	inline float NormalizeYawDegrees(double YawDegrees)
	{
		double Result = std::fmod(YawDegrees, 360.0);
		if (Result < 0.0)
		{
			Result += 360.0;
		}
		// A tiny negative Result + 360.0, or a double just under 360, can round to
		// exactly 360.0f on the cast; the wire range is the half-open [0, 360), so
		// fold 360 back to 0 (they are the same heading).
		float Out = static_cast<float>(Result);
		if (Out >= 360.0f)
		{
			Out = 0.0f;
		}
		return Out;
	}

	// Normalize an Unreal FRotator pitch (degrees, positive = looking up) to
	// [-90, 90]. Accepts [0,360) or signed input; folds to (-180,180] then clamps.
	inline float NormalizePitchDegrees(double PitchDegrees)
	{
		double P = std::fmod(PitchDegrees, 360.0);
		if (P < 0.0)
		{
			P += 360.0;
		}
		if (P > 180.0)
		{
			P -= 360.0;
		}
		if (P > 90.0)
		{
			P = 90.0;
		}
		if (P < -90.0)
		{
			P = -90.0;
		}
		return static_cast<float>(P);
	}
}
