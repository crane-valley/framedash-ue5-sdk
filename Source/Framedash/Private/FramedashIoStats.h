// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Engine-independent, thread-safe accumulator for synchronous disk-read
// activity. Feeds the io.* window metrics attached to perf_heartbeat events
// (metrics map / proto field 13). Two sources funnel into one process-wide
// accumulator: the IPlatformFile read wrapper
// (FFramedashIoTrackingPlatformFile, UE-coupled) counts one AddRead + AddOps
// per wrapped IFileHandle::Read; the manual Blueprint feed
// (UFramedashSubsystem::ReportIoSample) accumulates custom-loader / VFS reads
// the wrapper cannot see.
//
// The counters are CUMULATIVE and monotonic; reads are NON-destructive
// (ReadCumulative). Each consumer keeps its own last-seen baseline and computes
// window = cumulative - baseline. This is what keeps the source correct with
// multiple simultaneous GameInstances (e.g. multi-client PIE): a destructive
// drain would let whichever subsystem's heartbeat fired first steal the whole
// process window and leave the others reporting zero. With per-instance
// baselines, every instance honestly reports the same process-wide IO for its
// own heartbeat interval -- the correct semantic for a process-global source.
//
// This header is pure C++ on purpose: it includes NO Unreal headers (no
// CoreMinimal.h / FString / atomics-from-UE), so the engine-independent
// GoogleTest harness (sdks/ue5/Tests) can include and unit-test it without an
// UnrealEditor build -- same contract as FramedashFieldClamps.h. The
// IPlatformFile wrapper itself IS engine-coupled and is NOT host-testable.
//
// Fail-safe: values are clamped non-negative finite before accumulation, so a
// garbage read length or timing sample can never emit a non-finite / negative
// metric that would make the ingest server drop the whole flush.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Framedash
{
	// A snapshot of the monotonic cumulative counters (or a computed window delta
	// between two snapshots). Mirrors the three io.* metric keys: io.read_bytes,
	// io.read_time_ms, io.read_ops.
	struct FIoSnapshot
	{
		int64_t ReadBytes = 0;
		double ReadTimeMs = 0.0;
		int64_t ReadOps = 0;

		// Window delta = this snapshot minus an earlier baseline. Clamped at 0 per
		// field so a baseline that somehow observed a larger value (a torn read of
		// the independent atomics, or a future reset path) can never emit a negative
		// metric that the ingest server would reject. Cumulative counters only grow,
		// so in steady state this is a plain subtraction.
		FIoSnapshot Since(const FIoSnapshot& Baseline) const
		{
			FIoSnapshot Delta;
			Delta.ReadBytes = ReadBytes > Baseline.ReadBytes ? ReadBytes - Baseline.ReadBytes : 0;
			Delta.ReadOps = ReadOps > Baseline.ReadOps ? ReadOps - Baseline.ReadOps : 0;
			Delta.ReadTimeMs = ReadTimeMs > Baseline.ReadTimeMs ? ReadTimeMs - Baseline.ReadTimeMs : 0.0;
			return Delta;
		}
	};

	// Thread-safe, process-wide CUMULATIVE accumulator. The counters only ever
	// grow (AddRead/AddOps); reads are NON-destructive snapshots (ReadCumulative).
	// This is what makes the source safe to share across multiple simultaneous
	// GameInstances (e.g. multi-client PIE): a destructive drain would let
	// whichever subsystem's heartbeat fired first steal the whole process window
	// and leave the others reporting zero. Instead, each consumer keeps its OWN
	// last-seen baseline snapshot and computes window = cumulative - baseline, so
	// every instance honestly reports the same process-wide IO for its interval.
	// All mutators use relaxed atomics: the counters are independent sums with no
	// cross-field invariant, and consumers tolerate a read landing a hair before
	// or after a concurrent worker-thread add (that activity lands in the next
	// window); per-field Since() clamping absorbs any resulting tiny skew.
	class FIoStatsAccumulator
	{
	public:
		// Accumulate one completed read's byte count and wall time. Each field is
		// validated independently: a negative/zero byte count or a
		// non-finite/negative time is dropped (adds nothing) rather than poisoning
		// the sum. Does NOT count an operation -- callers pair this with AddOps so
		// a zero-byte read can still be counted as an op.
		void AddRead(int64_t Bytes, double TimeMs)
		{
			if (Bytes > 0)
			{
				SaturatingAdd(ReadBytes, Bytes);
			}
			if (std::isfinite(TimeMs) && TimeMs > 0.0)
			{
				AddTimeMs(TimeMs);
			}
		}

		// Count completed read operations. A non-positive count is dropped.
		void AddOps(int64_t Count)
		{
			if (Count > 0)
			{
				SaturatingAdd(ReadOps, Count);
			}
		}

		// Non-destructive snapshot of the monotonic cumulative totals. Callers keep
		// their own baseline and compute snapshot.Since(baseline) for the window.
		FIoSnapshot ReadCumulative() const
		{
			FIoSnapshot Snapshot;
			Snapshot.ReadBytes = ReadBytes.load(std::memory_order_relaxed);
			Snapshot.ReadOps = ReadOps.load(std::memory_order_relaxed);
			Snapshot.ReadTimeMs = ReadTimeMs.load(std::memory_order_relaxed);
			return Snapshot;
		}

	private:
		// SATURATING add: clamp at INT64_MAX instead of wrapping. A plain fetch_add
		// can overflow on extreme manual ReportIoSample values, and once an int64
		// counter wraps the cumulative contract breaks -- Since() would then compute
		// a signed subtraction across the wrap (UB) and yield a garbage/negative
		// window. Clamping keeps the counter monotonic (it only ever grows or holds
		// at MAX), so Since() stays well-defined and non-negative. Same
		// compare-exchange pattern as the double-time loop (Count is already > 0).
		static void SaturatingAdd(std::atomic<int64_t>& Counter, int64_t Amount)
		{
			constexpr int64_t Max = (std::numeric_limits<int64_t>::max)();
			int64_t Current = Counter.load(std::memory_order_relaxed);
			int64_t Desired;
			do
			{
				// Would Current + Amount overflow? (Amount > 0 by contract.) If the
				// headroom to Max is smaller than Amount, clamp; else it is safe to add.
				Desired = (Current > Max - Amount) ? Max : Current + Amount;
			} while (!Counter.compare_exchange_weak(
				Current, Desired, std::memory_order_relaxed));
		}

		// std::atomic<double> has no fetch_add before C++20, so accumulate via a
		// compare-exchange loop (the harness and plugin both build at C++17).
		void AddTimeMs(double TimeMs)
		{
			double Current = ReadTimeMs.load(std::memory_order_relaxed);
			double Desired;
			do
			{
				Desired = Current + TimeMs;
			} while (!ReadTimeMs.compare_exchange_weak(
				Current, Desired, std::memory_order_relaxed));
		}

		std::atomic<int64_t> ReadBytes{0};
		std::atomic<int64_t> ReadOps{0};
		std::atomic<double> ReadTimeMs{0.0};
	};

	// Process-wide accumulator shared by the (never-unchained) IPlatformFile
	// wrapper and the subsystem. An inline function-local static gives exactly
	// one instance across the module and outlives any GameInstance subsystem, so
	// the wrapper -- which is installed once and only disabled, never removed --
	// never references freed storage after Deinitialize.
	inline FIoStatsAccumulator& GlobalIoStats()
	{
		static FIoStatsAccumulator Instance;
		return Instance;
	}

	// Per-thread read-metering suppression depth. The SDK's OWN offline-queue
	// persistence (Project/Saved/Framedash/offline-queue.json) is read through the
	// same IPlatformFile wrapper that meters game reads; without this, restoring or
	// maintaining that queue would be reported as io.read_* -- a false disk-IO
	// spike exactly in sessions recovering telemetry. The persistence code brackets
	// its synchronous FFileHelper read with an FIoMeteringSuppressionScope; the
	// wrapper's OpenRead skips metering when the CURRENT thread's depth is > 0.
	// A thread-local counter (not a path comparison, which is fragile across
	// relative/absolute/case-normalized forms at this layer, and not a global flag,
	// which would also silence concurrent game reads on other threads) makes the
	// exclusion precise. Pure C++ so it is unit-testable in the GoogleTest harness.
	// The function-local static thread_local gives one per-thread slot shared across
	// translation units via the inline function's single definition.
	inline int& ThreadReadSuppressionDepth()
	{
		static thread_local int Depth = 0;
		return Depth;
	}

	inline bool IsThreadReadSuppressed()
	{
		return ThreadReadSuppressionDepth() > 0;
	}

	// RAII guard that suppresses read metering on the current thread for its
	// lifetime (nestable). Cheap when disk-IO tracking is off -- it only
	// increments/decrements a thread-local int.
	class FIoMeteringSuppressionScope
	{
	public:
		FIoMeteringSuppressionScope() { ++ThreadReadSuppressionDepth(); }
		~FIoMeteringSuppressionScope() { --ThreadReadSuppressionDepth(); }

		FIoMeteringSuppressionScope(const FIoMeteringSuppressionScope&) = delete;
		FIoMeteringSuppressionScope& operator=(const FIoMeteringSuppressionScope&) = delete;
	};
} // namespace Framedash
