// Copyright 2026 Crane Valley. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS

#include "Misc/AutomationTest.h"

#include "FramedashSessionManager.h"
#include "FramedashTypes.h"

namespace
{
	bool IsLowerHexDigit(const TCHAR Character)
	{
		return
			(Character >= TEXT('0') && Character <= TEXT('9')) ||
			(Character >= TEXT('a') && Character <= TEXT('f'));
	}

	bool HasUuidV7Shape(const FString& Value)
	{
		if (Value.Len() != 36)
		{
			return false;
		}

		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const bool bHyphenPosition =
				Index == 8 || Index == 13 || Index == 18 || Index == 23;
			if (bHyphenPosition)
			{
				if (Value[Index] != TEXT('-'))
				{
					return false;
				}
			}
			else if (!IsLowerHexDigit(Value[Index]))
			{
				return false;
			}
		}

		const TCHAR Variant = Value[19];
		return
			Value[14] == TEXT('7') &&
			(Variant == TEXT('8') || Variant == TEXT('9') ||
				Variant == TEXT('a') || Variant == TEXT('b'));
	}
}

BEGIN_DEFINE_SPEC(FFramedashSessionManagerSpec, "Framedash.Session.Manager",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FFramedashSessionManagerSpec)

void FFramedashSessionManagerSpec::Define()
{
	Describe("session identity", [this]()
	{
		It("creates a lowercase RFC 9562 UUIDv7", [this]()
		{
			const FFramedashSessionManager Manager;

			TestTrue(
				TEXT("session ID is a lowercase RFC 9562 UUIDv7"),
				HasUuidV7Shape(Manager.GetSessionId()));
		});

		It("creates a distinct ID for each manager instance", [this]()
		{
			const FFramedashSessionManager First;
			const FFramedashSessionManager Second;

			TestNotEqual(
				TEXT("separate launches receive distinct session IDs"),
				First.GetSessionId(),
				Second.GetSessionId());
		});
	});

	Describe("player identity", [this]()
	{
		It("normalizes the constructor value", [this]()
		{
			const FFramedashSessionManager Manager(TEXT(" \tplayer-42\r\n "));

			TestEqual(
				TEXT("constructor trims surrounding whitespace"),
				Manager.GetPlayerId(),
				FString(TEXT("player-42")));
		});

		It("updates the player without rotating the session", [this]()
		{
			FFramedashSessionManager Manager(TEXT("anonymous"));
			const FString SessionId = Manager.GetSessionId();
			const FString OverLimitPlayerId =
				FString::ChrN(FramedashConstants::MaxPlayerIdLength + 8, TEXT('x'));

			Manager.SetPlayerId(OverLimitPlayerId);

			TestEqual(
				TEXT("runtime player ID is capped to the ingest contract"),
				Manager.GetPlayerId().Len(),
				FramedashConstants::MaxPlayerIdLength);
			TestEqual(
				TEXT("player updates keep the launch session stable"),
				Manager.GetSessionId(),
				SessionId);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS && FRAMEDASH_WITH_AUTOMATION_SPECS
