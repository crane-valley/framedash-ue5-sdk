// Copyright 2026 Crane Valley. All Rights Reserved.
//
// Shared FString truncation that counts UTF-16 code units -- the unit the
// Framedash ingest validator enforces (it measures JavaScript string length
// after protobuf decode, i.e. UTF-16 code units). This must be platform-correct
// because UE's TCHAR width varies: on a 2-byte (UTF-16) TCHAR, FString::Len()
// already counts UTF-16 units; on a 4-byte (UTF-32) TCHAR, a non-BMP code point
// is ONE FString element but TWO UTF-16 units, so Len() under-counts and a string
// that passes a naive Len() clamp could still exceed the server cap and drop the
// whole batch. Counting by UTF-16 units here matches the Unity/Godot C# SDKs,
// whose strings are UTF-16 on every platform.

#pragma once

#include "CoreMinimal.h"

namespace Framedash
{
	/**
	 * Truncate Input to at most MaxUnits UTF-16 code units without splitting a
	 * surrogate pair. Returns "" for MaxUnits <= 0 and Input unchanged when it is
	 * already within budget.
	 */
	inline FString TruncateToUtf16Units(const FString& Input, int32 MaxUnits)
	{
		if (MaxUnits <= 0)
		{
			return FString();
		}
		// Fast path: on a UTF-16 TCHAR, Len() is exactly the UTF-16 unit count.
		if (sizeof(TCHAR) == 2 && Input.Len() <= MaxUnits)
		{
			return Input;
		}
		// Fast path: on a UTF-32 TCHAR, each element is at most 2 UTF-16 units, so
		// Len() <= MaxUnits / 2 is guaranteed within budget (skips the loop for
		// short strings on Linux/macOS).
		if (sizeof(TCHAR) == 4 && Input.Len() <= MaxUnits / 2)
		{
			return Input;
		}

		const int32 NumElements = Input.Len();
		int32 Units = 0;
		int32 Cut = 0;
		while (Cut < NumElements)
		{
			const uint32 Element = static_cast<uint32>(Input[Cut]);
			int32 Advance = 1; // FString elements consumed by this code point
			int32 Width = 1;   // UTF-16 code units contributed
			if (sizeof(TCHAR) == 2)
			{
				// UTF-16 element: a high surrogate followed by a low surrogate is a
				// 2-unit pair. A lone/malformed high surrogate counts as 1 unit so we
				// never skip a non-surrogate element from the budget.
				if (Element >= 0xD800u && Element <= 0xDBFFu && Cut + 1 < NumElements)
				{
					const uint32 Next = static_cast<uint32>(Input[Cut + 1]);
					if (Next >= 0xDC00u && Next <= 0xDFFFu)
					{
						Advance = 2;
						Width = 2;
					}
				}
			}
			else if (Element >= 0x10000u)
			{
				// UTF-32 element: a non-BMP code point is 2 UTF-16 units.
				Width = 2;
			}

			if (Units + Width > MaxUnits)
			{
				break;
			}
			Units += Width;
			Cut += Advance;
		}
		return Input.Left(Cut);
	}
}
