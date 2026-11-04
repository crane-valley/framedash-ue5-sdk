// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FFramedashSessionManager
{
public:
	explicit FFramedashSessionManager(const FString& PlayerId = TEXT(""));

	const FString& GetSessionId() const { return SessionId; }
	const FString& GetPlayerId() const { return PlayerId; }

	void SetPlayerId(const FString& NewPlayerId);

private:
	FString SessionId;
	FString PlayerId;
};
