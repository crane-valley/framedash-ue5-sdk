// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FramedashQuickstartActor.generated.h"

/**
 * Framedash in-editor quickstart actor (the activation LOGIC is editor-only).
 *
 * Goal: ACTIVATE your project (send its first real, map-qualified spatial event)
 * straight from the Unreal Editor's Play-in-Editor (PIE) session -- no packaged
 * build, no real players. The SDK's automatic performance heartbeat sends an
 * EMPTY map_id and does NOT count toward activation; only an explicit
 * Track(EventName, MapId) with a non-empty, REGISTERED map_id does. This actor
 * sends exactly that, once, on BeginPlay.
 *
 * This file is a copyable SAMPLE: it ships under the plugin's Samples/ folder and
 * is NOT part of the compiled Framedash module. To use the C++ path, copy this
 * pair into your own game (or plugin) module and add "Framedash" to that module's
 * Build.cs (see the sample README). Most projects should use the Blueprint recipe
 * in the README instead -- it needs no C++.
 *
 * Editor-only by design (mirrors the Unity sample): the configuration fields are
 * WITH_EDITORONLY_DATA and the activation logic is WITH_EDITOR, so both are
 * stripped from packaged (non-editor) builds -- this actor can never send
 * telemetry from a shipped game. The UCLASS shell (an inert AActor) is kept in
 * every build on purpose, so an instance left in a cooked level stays a valid
 * empty actor instead of becoming a missing-class reference. In a PIE session
 * (where WITH_EDITOR is defined) the full quickstart runs, which is the point.
 *
 * Fail-safe, matching the SDK contract: a missing field only logs a warning and
 * sends nothing, and nothing here throws out into the game.
 */
UCLASS()
class AFramedashQuickstartActor : public AActor
{
	GENERATED_BODY()

public:
	AFramedashQuickstartActor();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITORONLY_DATA
	/**
	 * An INGEST API key for your project -- it needs the events:write scope
	 * (dashboard -> project -> API keys -> new key, "Ingest" preset). A read/admin
	 * key without events:write is rejected by ingest and nothing appears. Required.
	 * Ignored if the SDK is already initialized elsewhere (the existing config wins).
	 */
	UPROPERTY(EditAnywhere, Category = "Framedash Quickstart")
	FString ApiKey;

	/**
	 * A map_id already registered in your project (dashboard -> Maps). The heatmap
	 * returns 404 for an unknown map, so this MUST be an existing map_id. Required.
	 */
	UPROPERTY(EditAnywhere, Category = "Framedash Quickstart")
	FString MapId;

	/** Event name to send. Any non-empty name works. */
	UPROPERTY(EditAnywhere, Category = "Framedash Quickstart")
	FString EventName = TEXT("quickstart_ping");
#endif

#if WITH_EDITOR
private:
	/** Sends one map-qualified event (the activation event) and flushes it immediately. */
	void SendActivationEvent();
#endif
};
