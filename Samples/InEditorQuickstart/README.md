# In-Editor Quickstart

Activate your Framedash project straight from the Unreal Editor's **Play-in-Editor
(PIE)** session -- no packaged build and no real players required.

Your project "activates" on its **first real, map-qualified spatial event**. The
SDK's automatic performance heartbeat sends an empty `map_id` and does **not**
count; only an explicit `Track(EventName, MapId)` with a non-empty, **registered**
`map_id` does. This quickstart sends exactly that.

There are two paths. **Blueprint** needs no C++ and works in any project --
start there. The **C++ actor** in this folder is the same flow for projects that
already have a C++ module.

## Before either path: register a map

In the Framedash dashboard open your project's **Maps** page and either click
**Generate demo** (fastest) or upload a map image. Copy one `map_id` from the
Maps list. The heatmap returns 404 for an unknown map, so the `map_id` must
already exist before you send.

You also need an **Ingest API key** with the `events:write` scope: dashboard ->
project -> API keys -> new key, **Ingest** preset. A read/admin key without
`events:write` is rejected by ingest and nothing appears.

## Path A: Blueprint (recommended, no C++)

> **Heads up:** unlike the C++ actor below, Blueprint logic is **not** stripped
> from packaged builds. Put this in a **throwaway test level** (or a temporary
> actor you delete after activating), so a `BeginPlay` graph you forgot to remove
> never initializes the SDK and sends `quickstart_ping` from real players in a
> shipped game. For a path that is guaranteed inert in builds, use the C++ actor.

1. Make sure the plugin is enabled (**Edit > Plugins > Framedash**) and your
   project has restarted.
2. Open any level (a throwaway test level is fine). Open the **Level Blueprint**
   (**Blueprints > Open Level Blueprint**), or create an **Actor Blueprint** and
   place one in the level.
3. From **Event BeginPlay**, build this node chain:
   - **Get Framedash Subsystem** (search the palette for "Framedash"; it is a
     Game Instance Subsystem getter that returns the subsystem instance).
   - Drag off it -> **Is Initialized**. Feed it into a **Branch**.
   - On the **False** branch, call **Initialize Telemetry** with `Api Key` set to
     your Ingest key. (Skip this when already initialized so you do not stack a
     duplicate-init warning -- the node simply no-ops if you do call it.)
   - Call **Set Event Sampling Rate** (`quickstart_ping`, `1.0`). This forces the
     ping past sampling, so it still activates in a project whose global
     `SamplingRate` was lowered (a test project may set it to 0); without it,
     `Track` uses the default `Player` source and the event can be dropped before
     it reaches the heatmap.
   - Call **Track**: `Event Name` = `quickstart_ping`, `Map Id` = the `map_id`
     you copied, `Position` = any vector inside the map's bounds (the demo maps
     contain the origin, so `0,0,0` works). The non-empty `Map Id` is what makes
     the event activate your project.
   - Call **Flush** so the point reaches the heatmap in seconds.
4. Press **Play** (PIE). Your project activates.
5. Open that map's **heatmap** in the dashboard -- your point appears.

## Path B: C++ actor (this folder)

This `Samples/` folder is **not compiled** by the plugin, so copy the actor into
a module you build:

1. Copy `FramedashQuickstartActor.h` and `FramedashQuickstartActor.cpp` into your
   own game (or plugin) module's `Source` tree.
2. Add `Framedash` to that module's `Build.cs`:
   ```csharp
   PrivateDependencyModuleNames.Add("Framedash");
   ```
   (Blueprint-only projects have no C++ module -- use Path A instead, or convert
   the project to C++ via **Tools > New C++ Class** first.)
3. Compile, then drag **Framedash Quickstart Actor** into a level.
4. In its **Details** panel set **Api Key** (Ingest key) and **Map Id** (the
   registered `map_id`). **Event Name** defaults to `quickstart_ping`.
5. Press **Play** (PIE). The actor sends one map-qualified event on BeginPlay and
   your project activates. Open that map's heatmap to see the point.

## Notes

- **Real events:** this emits real telemetry (not the dashboard "Generate demo"
  synthetic data), so it counts toward activation -- which synthetic demo data
  does not.
- **Editor-only logic (C++ actor):** the configuration fields are
  `WITH_EDITORONLY_DATA` and the activation logic is `WITH_EDITOR`, so both are
  stripped from packaged (non-editor) builds -- the actor can never send telemetry
  from a shipped game. The `UCLASS` shell is kept in every build on purpose, so an
  instance left in a cooked level stays a valid inert actor instead of becoming a
  missing-class reference. It runs fully in a PIE session.
- **Fail-safe:** a missing `Map Id` (always required), or a missing `Api Key` when
  the SDK is not already initialized, only logs a warning (and sends no telemetry at
  all); nothing here throws into your game. When the SDK is already initialized, the
  `Api Key` may be left blank.
- **Sampling:** both paths set a per-event sampling override of `1.0` for the
  quickstart event before sending (the C++ actor does it in code; the Blueprint
  recipe includes a **Set Event Sampling Rate** node), so the ping activates even
  in a project whose global `SamplingRate` is below 1 (a test project may set it
  to 0). Without it, the default `Player`-source event could be dropped by sampling
  and the project would never activate.
- **Position / map bounds:** the event is sent at the actor's location, so it must
  fall inside your map's world bounds to show on the heatmap. The demo maps contain
  the origin, so an actor at `(0,0,0)` works; for your own uploaded map, place the
  actor within that map's bounds, and move or duplicate it to spread points.
- **Already set up?** If the SDK is already initialized elsewhere (auto-init via
  project settings, or another bootstrap), the actor's `Api Key` is ignored -- the
  existing configuration wins -- and it logs a warning. Use the quickstart in a
  project where the SDK is not yet initialized to exercise this key.
- This is a quickstart, not a production integration. For the full setup (player
  identity, build ids, automatic performance capture, sampling) see the plugin
  README and https://docs.framedash.dev/en/sdk/unreal/.
