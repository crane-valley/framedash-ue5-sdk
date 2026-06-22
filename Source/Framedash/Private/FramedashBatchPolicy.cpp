// Copyright Crane Valley. All Rights Reserved.

#include "FramedashBatchPolicy.h"

namespace Framedash
{

bool FBatchPolicy::ExceedsWireCaps(int32_t EventCount, int64_t DecodedEntryCount)
{
	if (EventCount < 0) EventCount = 0;
	if (DecodedEntryCount < 0) DecodedEntryCount = 0;

	// A batch of one (or zero) is never split: a single oversized event is
	// bounded by the payload-byte path or dropped on a 413, and the server
	// enforces the per-event attribute/metric caps that splitting cannot fix.
	if (EventCount <= 1) return false;

	return EventCount > MaxEventsPerBatch
		|| DecodedEntryCount > static_cast<int64_t>(MaxDecodedEntries);
}

} // namespace Framedash
