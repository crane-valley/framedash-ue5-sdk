// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

// Engine-independent: this header MUST NOT include UE5 headers
// (CoreMinimal.h, Containers/*, etc.). Pure C++ so it can be
// compiled into the standalone GoogleTest harness under sdks/ue5/Tests
// without an UnrealEditor build.

#include <cstdint>

namespace Framedash
{

// Pure batch-sizing policy for the telemetry transport. No UE5 dependencies,
// so it is unit-testable under GoogleTest (FFramedashTransport itself is
// engine-coupled via TArray/IHttpRequest and excluded from the test harness).
//
// The split decision keys off the SERVER per-request caps, not the per-flush
// batch threshold: a normal sub-cap drain is sent as a single request (then
// bounded only by the payload-byte limit), so a stall/burst drain is not
// fragmented into many tiny requests.
//
// Mirrors Unity SDK BatchPolicy semantics exactly:
//   - <=1 event: never split (a single oversized event is bounded by the
//     payload-byte path or dropped on a 413; the server enforces per-event
//     attribute/metric caps that splitting cannot fix)
//   - events > MaxEventsPerBatch OR decodedEntries > MaxDecodedEntries: split
//
// ExceedsWireCaps takes counts, not engine event structs, so subsystem code
// computes the decoded-entry count before calling this function.
class FBatchPolicy
{
public:
	// Server-side per-request event cap (mirrors
	// packages/ingest-core/src/config.ts MAX_EVENTS_PER_BATCH).
	// The consumer rejects a batch with more events than this wholesale.
	static constexpr int32_t MaxEventsPerBatch = 10000;

	// Server-side per-request decoded-object cap (mirrors
	// packages/ingest-core/src/config.ts MAX_DECODED_ENTRIES):
	// events PLUS every attributes/metrics map entry across all events.
	// The consumer rejects a batch whose total exceeds this wholesale,
	// even when the event count, per-event counts, and payload size are
	// each within their own limits (e.g. 10,000 events x 10 attrs =
	// 110,000 entries > cap).
	static constexpr int32_t MaxDecodedEntries = 100000;

	// Whether the batch must be chunked before sending because it would be
	// rejected wholesale by a server per-request cap.
	//
	// EventCount   -- number of events in the batch.
	// DecodedEntryCount -- events + all attributes/metrics map entries.
	//
	// Returns false for batches of 0 or 1: a single oversized event cannot
	// be split further and must reach the payload-byte path or 413 handling.
	// Negative inputs are treated as 0.
	static bool ExceedsWireCaps(int32_t EventCount, int64_t DecodedEntryCount);
};

} // namespace Framedash
