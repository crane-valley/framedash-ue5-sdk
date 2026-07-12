// Copyright Crane Valley. All Rights Reserved.
//
// Engine-independent state machine + math for the map/level load-time helper
// (UFramedashSubsystem::BeginMapLoad / EndMapLoad / ReportMapLoad). Holds the
// pending measurement's monotonic start timestamp (seconds) and computes the
// elapsed load time in milliseconds. The subsystem reads the engine wall clock
// (FPlatformTime::Seconds) and feeds the seconds in, so the timing math is
// unit-testable without an engine.
//
// This header is pure C++ on purpose: it includes NO Unreal headers (no
// CoreMinimal.h / FString), so the engine-independent GoogleTest harness
// (sdks/ue5/Tests) can include and unit-test it without an UnrealEditor build --
// same contract as FramedashIoStats.h / FramedashFieldClamps.h. The loaded map
// NAME (an FString) is held by the subsystem, not here, so this stays UE-free;
// this class owns only the pending flag + start timestamp.
//
// Fail-safe: End() floors the elapsed time at 0 so a backwards monotonic-clock
// reading never yields a negative load time, and IsValidLoadTimeMs rejects a
// NaN/Infinity/negative direct report so a garbage value is dropped (not clamped).

#pragma once

#include <cmath>

namespace Framedash
{
	// The auto event name and metrics-map key are shared with the Unity/Godot SDKs:
	// the load time rides the existing metrics map (proto field 13) as
	// "load_time_ms" on a "map_load" event, with the loaded map name carried as
	// attributes["map_name"] and map_id left EMPTY (keeps the event out of the
	// spatial heatmap grid query and the activation gate, which key on map_id)
	// -- no proto/nanopb/ClickHouse change (mirrors the io.* attributes-map guardrail).
	// (The literals are used at the subsystem call site as UE FString/TEXT; kept here
	// as documentation of the cross-SDK contract this timer feeds.)

	// Pending-measurement timer for a single in-flight map/level load. Not
	// thread-safe by itself: the subsystem calls Begin/End on the game thread
	// (loading), matching where the other subsystem tracking state lives.
	class FMapLoadTimer
	{
	public:
		// Begin (or replace) a pending measurement. Calling Begin again before End
		// REPLACES the pending measurement -- the earlier start is discarded and only
		// the most recent Begin/End pair is reported.
		void Begin(double StartSeconds)
		{
			StartSeconds_ = StartSeconds;
			bPending_ = true;
		}

		// Complete a pending measurement and clear it. Returns false (a no-op) when no
		// Begin is pending. On success OutElapsedMs is the load time in milliseconds,
		// floored at 0 so a backwards monotonic-clock reading never yields a negative
		// load time.
		bool End(double EndSeconds, double& OutElapsedMs)
		{
			if (!bPending_)
			{
				OutElapsedMs = 0.0;
				return false;
			}
			const double Ms = (EndSeconds - StartSeconds_) * 1000.0;
			OutElapsedMs = Ms < 0.0 ? 0.0 : Ms;
			bPending_ = false;
			return true;
		}

		// True while a Begin is awaiting its End.
		bool HasPending() const { return bPending_; }

		// Reset any pending measurement (called on (re-)init so a fresh session never
		// completes a load begun by a prior session).
		void Reset() { bPending_ = false; }

		// Validate a directly-reported load time (ReportMapLoad). A NaN, Infinity, or
		// negative value is rejected so the whole call is DROPPED (not clamped),
		// matching the drop-don't-clamp rule the manual metric feeds use.
		static bool IsValidLoadTimeMs(double LoadTimeMs)
		{
			return std::isfinite(LoadTimeMs) && LoadTimeMs >= 0.0;
		}

	private:
		double StartSeconds_ = 0.0;
		bool bPending_ = false;
	};
} // namespace Framedash
