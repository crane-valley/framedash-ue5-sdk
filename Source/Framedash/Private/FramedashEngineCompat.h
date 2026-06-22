// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "Misc/EngineVersionComparison.h"

// TArray::RemoveAt / RemoveAtSwap took a `bool bAllowShrinking` argument until
// UE 5.4, which replaced it with the EAllowShrinking enum (the bool overload is
// deprecated on 5.4+). Use this token at the no-shrink call sites so they stay
// buildable from UE 5.3 through the current release without hitting either the
// missing enum (pre-5.4) or the deprecated bool overload (5.4+).
#if UE_VERSION_OLDER_THAN(5, 4, 0)
	#define FRAMEDASH_ALLOW_SHRINKING_NO false
#else
	#define FRAMEDASH_ALLOW_SHRINKING_NO EAllowShrinking::No
#endif
