// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Manages session ID (per-launch) and player ID (developer-supplied). */
class FFramedashSessionManager
{
public:
	explicit FFramedashSessionManager(const FString& PlayerId = TEXT(""));

	const FString& GetSessionId() const { return SessionId; }
	const FString& GetPlayerId() const { return PlayerId; }

	/** Update the player ID at runtime (e.g. after login). Must be called from GameThread. */
	void SetPlayerId(const FString& NewPlayerId);

private:
	FString SessionId;
	FString PlayerId;
};
