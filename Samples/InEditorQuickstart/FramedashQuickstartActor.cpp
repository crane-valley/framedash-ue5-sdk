// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashQuickstartActor.h"

#if WITH_EDITOR
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FramedashSubsystem.h"

// A file-local log category so the quickstart's messages are filterable on their
// own. It is intentionally NOT the plugin's LogFramedash: that category is
// declared without FRAMEDASH_API, so referencing it from this sample once it has
// been copied into a different game/plugin module would not link in a modular
// (editor) build -- and this logic is editor-only, i.e. always a modular build.
DEFINE_LOG_CATEGORY_STATIC(LogFramedashQuickstart, Log, All);
#endif

AFramedashQuickstartActor::AFramedashQuickstartActor()
{
	// The quickstart sends a single event on BeginPlay; no per-frame work is needed.
	PrimaryActorTick.bCanEverTick = false;
}

void AFramedashQuickstartActor::BeginPlay()
{
	Super::BeginPlay();

	// In a packaged (non-editor) build WITH_EDITOR is 0, so BeginPlay only calls
	// Super -- the actor is inert and emits no telemetry. In a PIE session it runs
	// the activation logic below.
#if WITH_EDITOR
	SendActivationEvent();
#endif
}

#if WITH_EDITOR
void AFramedashQuickstartActor::SendActivationEvent()
{
	// Trim values pasted from the dashboard so stray whitespace still works; the
	// serialized Inspector fields are left untouched. Track() drops a whitespace
	// event_name, so fall back to the default to keep the "Activated" log honest.
	const FString TrimmedApiKey = ApiKey.TrimStartAndEnd();
	const FString TrimmedMapId = MapId.TrimStartAndEnd();
	const FString TrimmedEventName = EventName.TrimStartAndEnd();
	const FString ResolvedEventName = TrimmedEventName.IsEmpty() ? TEXT("quickstart_ping") : TrimmedEventName;

	// map_id is ALWAYS required: a non-empty, registered map_id is what makes the
	// event map-qualified (the event that activates the project), and the heatmap
	// 404s on an unknown map. Validate it before doing anything else.
	if (TrimmedMapId.IsEmpty())
	{
		UE_LOG(LogFramedashQuickstart, Warning,
			TEXT("Set 'Map Id' to a map registered in your project (dashboard -> Maps), "
				 "then press Play."));
		return;
	}

	const UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UFramedashSubsystem* Framedash =
		GameInstance ? GameInstance->GetSubsystem<UFramedashSubsystem>() : nullptr;
	if (Framedash == nullptr)
	{
		UE_LOG(LogFramedashQuickstart, Warning,
			TEXT("Framedash subsystem unavailable -- is the plugin enabled for this project?"));
		return;
	}

	if (Framedash->IsInitialized())
	{
		// Already initialized (auto-init via project settings, or another bootstrap).
		// The Api Key is not needed here, so this path works even with the actor's Api
		// Key left blank -- the event is sent with the existing configuration and the
		// actor's Api Key is intentionally ignored.
		UE_LOG(LogFramedashQuickstart, Warning,
			TEXT("The SDK is already initialized, so this actor's 'Api Key' is ignored -- "
				 "the event is sent with the existing configuration."));
	}
	else
	{
		// Only require the Api Key when we actually have to initialize. Validate it
		// BEFORE InitializeTelemetry, which would otherwise immediately send a real
		// session_start and start the perf heartbeat: a missing key must emit nothing.
		if (TrimmedApiKey.IsEmpty())
		{
			UE_LOG(LogFramedashQuickstart, Warning,
				TEXT("Set 'Api Key' (an Ingest key with the events:write scope) on this "
					 "actor, then press Play."));
			return;
		}

		// A stable demo player_id keeps the events attributed and skips the SDK's
		// empty-player_id warning; a real integration calls SetPlayerId with its own id.
		// Endpoint and build id are left empty so the SDK uses its production defaults.
		Framedash->InitializeTelemetry(TrimmedApiKey, TEXT(""), TEXT(""), TEXT("quickstart-player"));
	}

	// Force this one activation ping past sampling. If the project lowered the global
	// SamplingRate (a test project might set it to 0), the default Player-source event
	// would otherwise be droppable and the project would never activate despite the
	// log below. A per-event override of 1.0 guarantees this event is sent; automatic
	// events already bypass sampling, but this is an explicit Player event.
	Framedash->SetEventSamplingRate(ResolvedEventName, 1.0f);

	// The NON-EMPTY map_id is what makes this event map-qualified -- the event that
	// activates the project. GetActorLocation gives the heatmap a spatial point; place
	// this actor inside the map's world bounds (the demo maps contain the origin, so an
	// actor at the origin works) and move or duplicate it to spread points out.
	Framedash->Track(ResolvedEventName, TrimmedMapId, GetActorLocation());

	// Quickstart only: flush immediately so the point reaches the heatmap in seconds
	// instead of waiting for the periodic flush. A real integration lets the SDK batch
	// (it flushes on its own at 100 events / 30s / shutdown).
	Framedash->Flush();

	UE_LOG(LogFramedashQuickstart, Log,
		TEXT("Activated: sent '%s' on map '%s'. Open that map's heatmap in the dashboard "
			 "to see the point."),
		*ResolvedEventName, *TrimmedMapId);
}
#endif
