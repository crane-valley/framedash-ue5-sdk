# Framedash UE5 SDK

Unreal Engine 5 plugin for collecting game telemetry and sending it to the Framedash platform.

## Requirements

- Unreal Engine 5.3 or newer. This is a source plugin that compiles against the Unreal Engine version of your project. Release zips are published per engine version (`-ue5.3` ... `-ue5.8`) with `EngineVersion` pinned and prebuilt Win64 editor binaries -- install the zip matching your engine. Building from the repository source instead works on any supported 5.x (the source descriptor does not pin `EngineVersion`). Build-verified on UE 5.3, 5.4, 5.5, and 5.6. Note: the credential-leaking cross-origin redirect detection in the transport requires `IHttpBase::GetEffectiveURL` (UE 5.4+); on UE 5.3 that one defense is compiled out, and the rest of the SDK is unaffected.

## Structure

```
sdks/ue5/                     # Standalone redistributable plugin (drop into Plugins/Framedash/)
  Framedash.uplugin
  Source/Framedash/
    Public/                   # Public headers
    Private/                  # Implementation (Proto/, ThirdParty/nanopb/)
  Resources/
  Tests/                      # Engine-independent unit tests (GoogleTest)
```

## Plugin Modules

| File | Description |
|------|-------------|
| `FramedashSubsystem.h/cpp` | Game Instance Subsystem — main entry point, lifecycle management |
| `FramedashSettings.h/cpp` | Project settings (API key, endpoint, sampling config) |
| `FramedashTypes.h` | Shared type definitions |
| `FramedashPerformanceCollector` | Automatic frame time, GPU time, memory collection via `stat unit` globals |
| `FramedashEventBuffer` | Batched event buffering |
| `FramedashPersistenceProvider` | Offline queue persistence for unsent events |
| `FramedashSessionManager` | Session ID and device metadata |
| `FramedashSamplingPolicy` | Configurable event sampling |
| `FramedashTransport` | HTTPS transport with Protobuf serialization |

## Proto Generation

The UE5 SDK uses [nanopb](https://github.com/nanopb/nanopb) to serialize Protobuf payloads. The generated C code (`telemetry.pb.h`, `telemetry.pb.c`) is committed to the repo — you only need to regenerate after changing the canonical proto schema.

**Single source of truth**: `packages/proto/framedash/v1/telemetry.proto`. The SDK does **not** have its own `.proto` copy.

### Regenerating nanopb Code

```bash
# Prerequisites (one-time)
pip install nanopb==0.4.9.1

# Generate
pnpm --filter @framedash/proto generate:nanopb

# Verify generation matches committed files
pnpm --filter @framedash/proto check-freshness:nanopb
```

The `.options` file (`Private/Proto/telemetry.options`) controls nanopb field types (FT_CALLBACK for strings/maps, FT_STATIC for scalars). Edit this file if new dynamic-length fields are added to the proto.

## Automatic Events

The SDK automatically sends the following events with `Source=Automated`. These bypass sampling policy and always fire regardless of the configured sampling rate.

| Event | Trigger | Description |
|-------|---------|-------------|
| `session_start` | Once, on initialization | Guarantees the backend sees at least one event per session |
| `perf_heartbeat` | Every 10 seconds (via `Tick()`) | Continuous performance baseline (FPS, frame time, game thread, render thread, GPU time, memory) |

Both events include full performance metrics from `FramedashPerformanceCollector`.

## Offline Queue

The SDK persists unsent events to `Project/Saved/Framedash/offline-queue.json`
when the game shuts down with buffered telemetry or when a transient send
failure exhausts retries. The queue is restored on the next SDK initialization
and replayed through the normal batch path.

Offline queueing is enabled by default. To disable disk persistence, set:

```ini
[/Script/Framedash.FramedashSettings]
bEnableOfflineQueue=False
```

The persisted queue is capped at 1,000 events; when full, the oldest persisted
events are dropped first so gameplay is never blocked by telemetry.

## Performance Collection

Uses `FApp::GetDeltaTime()` for wall-clock frame time. GPU time is collected via `RHIGetGPUFrameCycles()` and converted to milliseconds. Game thread and render thread timing use `GGameThreadTime` / `GRenderThreadTime` globals from the RenderCore module. See [Frame Timing Metrics Guide](../../docs/en/frame-timing-metrics.md) for details on available metrics and collection APIs.

## Field Limits

Every event field is clamped client-side to the Framedash ingest limits before
it is buffered. The ingest validator rejects an entire batch if any single field
is out of range (a 202 is returned first, then the batch is dropped during
processing), so the SDK normalizes each field so one bad value cannot lose
unrelated events in the same flush.

| Field | Limit | Over-limit handling |
|-------|-------|---------------------|
| `event_name` | 1–128 chars | Empty/whitespace-only dropped; longer truncated to 128 |
| `map_id` | ≤ 128 chars | Truncated to 128 |
| `build_id` | ≤ 128 chars | Truncated to 128 (once, at initialization) |
| `player_id` | ≤ 128 chars | Trimmed, then truncated to 128 |
| Position `x`/`y`/`z` | finite, \|v\| ≤ 1e9 | NaN/Inf → 0; magnitude clamped to ±1e9 |
| Attributes | ≤ 50 entries | Extra entries and empty keys dropped |
| Attribute key / value | ≤ 64 / ≤ 512 chars | Truncated |
| Metrics | ≤ 50 entries | Extra entries, empty keys, and NaN/Inf values dropped |
| Metric key | ≤ 64 chars | Truncated |
| `fps` | 0–1000 | Clamped (NaN/negative → 0) |
| `frame_time_ms` / `gpu_time_ms` / `game_thread_ms` / `render_thread_ms` | 0–10000 | Clamped (NaN/negative → 0) |
| `memory_used_bytes` | 0–64 GiB | Negative → 0; above 64 GiB capped |
| `platform` / `engine_version` | ≤ 64 chars | Auto-collected; truncated (a custom engine branch name can exceed the cap) |

Truncation counts UTF-16 code units (via `TruncateToUtf16Units`), matching the
server's string-length semantics on every platform (UE's `TCHAR` is 2 bytes on
Windows but 4 bytes on Linux/macOS, so a raw `FString::Len()`/`Left()` would
under-count non-BMP characters). These caps mirror the Godot and Unity SDKs and
stay in sync with `packages/ingest-core/src/config.ts`.

## Camera Direction

When **Capture Camera Rotation** is enabled (the default), every event records the local player's camera yaw and pitch, which powers the direction breakdown on the heatmap cell-detail view. The SDK samples the active `PlayerCameraManager` rotation each tick on the game thread and stamps events from that cached snapshot; like all SDK methods, `Track()` is intended to be called on the game thread. Capture is skipped on dedicated servers and before a local `PlayerController` exists, so it never affects headless builds. Yaw is normalized to `[0, 360)` and increases clockwise; the direction chart labels yaw 0 as North, with the engine's forward axis as that reference (a game world has no geographic North, so the compass labels are relative). Pitch is `[-90, 90]` (+90 = looking up).

Disable it under **Project Settings > Plugins > Framedash > Capture Camera Rotation**, or set `bCaptureCameraRotation=False` in `Config/DefaultGame.ini`.

## Disk I/O Metrics

When **Track Disk IO** is enabled (`bTrackDiskIo`, **off by default**), the SDK chains a lightweight `IPlatformFile` wrapper at initialization that counts synchronous disk reads, and attaches the window totals since the previous heartbeat as metrics on each `perf_heartbeat` event:

| Metric key | Meaning |
|------------|---------|
| `io.read_bytes` | Bytes read since the last heartbeat |
| `io.read_time_ms` | Wall time spent in reads, in milliseconds |
| `io.read_ops` | Number of read operations |

These ride in the generic metrics map (no proto/schema change), so query them via the data-export / query REST API (e.g. `metrics['io.read_bytes']`). First-class `perf-diff` build-over-build comparison of `io.*` is Phase 2; the `framedash perf-diff` / builds-compare API currently compares only `frame_time`, `memory`, and `gpu_time`. The keys are attached **only** when the session enabled a collection source -- `bTrackDiskIo` was on, or `ReportIoSample` was accepted this session -- and **only** on `perf_heartbeat` events; an absent key means "not collected" (distinct from a collected `0`).

The counters are process-wide and cumulative: each heartbeat reports the read volume in that window (cumulative minus the previous heartbeat's baseline). With multiple simultaneous `GameInstance`s (e.g. multi-client PIE) each instance keeps its own baseline, so every instance reports the same process-wide I/O for its own interval rather than one instance draining the window from the others.

The attach decision is **session-scoped**, not process-lifetime: in a single process that runs several telemetry sessions (typical of the UE Editor / PIE), a later session with disk-IO tracking off and no manual feed emits **no** `io.*` keys even if an earlier session in the same process collected I/O.

Enable it under **Project Settings > Plugins > Framedash > Track Disk IO**, or set `bTrackDiskIo=True` in `Config/DefaultGame.ini`. It is opt-in because wrapping the platform file layer is invasive; a failed install is swallowed and never disrupts the game. The wrapper is installed once and is never unchained -- shutting a subsystem down only stops counting once the last instance that enabled it shuts down (enablement is reference-counted).

**IoDispatcher / Nanite bypass caveat:** the IoDispatcher/IoStore path (zen loader, Nanite streaming, bulk data) and async reads bypass `IPlatformFile` on some platforms, so the automatic counters **undercount** Nanite-heavy or async-streamed I/O. Nanite-specific streaming metrics are deferred to a later phase.

**Self-exclusion caveat:** the SDK's own offline-queue file (`Project/Saved/Framedash/offline-queue.json`) is read through the same wrapper, so the SDK excludes its own persistence reads from the counters (via a thread-local suppression scope around the read). This prevents a false `io.read_*` spike in sessions that recover persisted telemetry; those reads are therefore not reflected in `io.*`.

### Manual feed (`ReportIoSample`)

For Shipping builds, custom loaders, or a virtual file system the wrapper cannot see, report reads yourself. This accumulates into the same heartbeat window and works **regardless** of the `bTrackDiskIo` setting:

```cpp
if (UFramedashSubsystem* Framedash = GetGameInstance()->GetSubsystem<UFramedashSubsystem>())
{
    // Bytes read, wall time in ms, and number of read operations for a load.
    Framedash->ReportIoSample(/*Bytes=*/1048576, /*ReadTimeMs=*/12.5f, /*Ops=*/8);
}
```

A sample with any negative or non-finite component is dropped in full (no accumulation, no io.* activation) -- same contract as the Unity and Godot SDKs -- so a bad sample can neither poison the window nor activate misleading zero windows.

## Memory Detail Metrics

When **Track Memory Detail** is enabled (`bTrackMemoryDetail`, **off by default**), the SDK samples memory-category usage at the heartbeat cadence and attaches it as `mem.*` metrics on each `perf_heartbeat` event:

| Metric key | Meaning | Source |
|------------|---------|--------|
| `mem.vram` | Video memory in use, in bytes (streaming + non-streaming texture allocations) | RHI texture memory stats -- always available |
| `mem.textures` | LLM `Textures` tag bytes | Low-Level Memory tracker -- only with `-llm` |
| `mem.meshes` | LLM `Meshes` tag bytes | Low-Level Memory tracker -- only with `-llm` |
| `mem.audio` | LLM `Audio` tag bytes | Low-Level Memory tracker -- only with `-llm` |

`mem.vram` is read from the RHI (`RHIGetTextureMemoryStats`) and works on every RHI without any special launch flag; it reports the sum of streaming and non-streaming **texture** allocations (the closest RHI proxy for live VRAM footprint -- not the hardware capacity or the streaming-pool budget). It is omitted on headless / `-nullrhi` builds where no RHI is present.

The `mem.textures` / `mem.meshes` / `mem.audio` breakdown requires the **Low-Level Memory tracker** to be both compiled into the build **and** enabled at runtime (launch the engine with `-llm`). When LLM is unavailable or disabled, only `mem.vram` is emitted. A category whose tag reports no tracked bytes stays **absent** (an absent key means "not collected", distinct from a collected `0`).

These ride in the generic metrics map (no proto/schema change), so query them via the data-export / query REST API (e.g. `metrics['mem.vram']`). They are attached when the session enabled the setting; the attach decision is **session-scoped** (a later session in the same process with the setting off emits no `mem.*`). Sampling reads cached RHI/LLM counters at the 10s heartbeat cadence, so it adds no per-frame work.

**Where the keys attach (spatial heatmap).** `perf_heartbeat` has no world position (empty `map_id`), so the spatial heatmap grid -- which buckets by `map_id` and cell bounds -- never sees the heartbeat's metrics. To make per-cell memory heatmaps work, the SDK attaches the same `mem.*` sample to two places:

- every `perf_heartbeat` (as with `io.*`), for time-series / `metrics[...]` queries; and
- every **position-qualified** tracked event (any event you send with a non-empty `map_id`), so the grid can aggregate memory per cell.

Position-qualified events carry a **cached** sample (refreshed at the 10s heartbeat cadence, plus one lazy sample taken on the first qualifying event so the initial window is not blind) -- no engine sampling or string building happens per event. Events with an empty `map_id` (other than `perf_heartbeat`) get nothing. If you pass your own `mem.*` key in a `TrackWithData` metrics map, **your value wins** (the SDK-injected key is overridden). Absent categories stay absent on events exactly as on the heartbeat.

Enable it under **Project Settings > Plugins > Framedash > Track Memory Detail**, or set `bTrackMemoryDetail=True` in `Config/DefaultGame.ini`. It is opt-in (default OFF), so default sessions keep the zero-allocation event path untouched. A sampler failure degrades to an absent key and never disrupts the game (fail-safe).

## Map/Level Load-Time

Measure how long a level takes to load and emit it as a `map_load` event. The load time rides the generic metrics map (`load_time_ms`) and the loaded map name rides the attributes map as `attributes["map_name"]`. `map_id` is left **empty** on purpose (like `perf_heartbeat`): a `map_load` has no world position, so an empty `map_id` keeps it out of the spatial heatmap and the activation gate, which key on a non-empty `map_id`. There is no dedicated proto or ClickHouse column yet (web/CLI charts, grouped by `attributes['map_name']`, and `perf-diff` gating land in a follow-up PR). Query it today via the data-export / query REST API (e.g. `metrics['load_time_ms']`). The event flows through the normal `Track` path, so it is sampled and buffered like any other event. All three methods are Blueprint-callable, fail-safe (never disrupt the game), no-ops before initialization, and **game-thread only** (the Track path reads game-thread-only perf state -- dispatch back to the game thread if a custom loader completes on a worker thread).

```cpp
if (UFramedashSubsystem* Framedash = GetGameInstance()->GetSubsystem<UFramedashSubsystem>())
{
    // Time a load with the built-in timer:
    Framedash->BeginMapLoad(TEXT("world_1"));
    // ... load the level ...
    Framedash->EndMapLoad();   // emits map_load (map_name="world_1", load_time_ms=elapsed)

    // Or report a time you measured yourself (custom/streaming loaders):
    Framedash->ReportMapLoad(TEXT("world_1"), /*LoadTimeMs=*/842.0);
}
```

The timer uses a monotonic wall clock (`FPlatformTime::Seconds`), so a paused game or time dilation does not distort the measurement. Calling `BeginMapLoad` again before `EndMapLoad` replaces the pending measurement; `EndMapLoad` with no pending `BeginMapLoad` is a no-op. A NaN/Infinity/negative `ReportMapLoad` time is dropped (not clamped).

## Quick Start

### 1. Add Plugin to Your Project

Copy the plugin (the `sdks/ue5/` folder) into your project as `Plugins/Framedash/`, then add to your `.uproject`:

```json
{
  "Plugins": [{ "Name": "Framedash", "Enabled": true }]
}
```

### 2. Add Module Dependency

In your module's `Build.cs`:

```csharp
PrivateDependencyModuleNames.Add("Framedash");
```

> **Note:** Without this step, `#include "FramedashSubsystem.h"` will fail to compile. Enabling the plugin in `.uproject` alone is not enough.

### 3. Initialize

**Option A — Auto Initialize (recommended):** Add to `Config/DefaultGame.ini`:

```ini
[/Script/Framedash.FramedashSettings]
ApiKey=your-api-key
bAutoInitialize=True
```

No C++ code required. The subsystem reads these settings on startup.

These settings are also editable in **Project Settings > Plugins > Framedash**.

**Option B — Manual Initialize from C++:**

```cpp
#include "FramedashSubsystem.h"

if (auto* Subsystem = GetGameInstance()->GetSubsystem<UFramedashSubsystem>())
{
    Subsystem->InitializeTelemetry(TEXT("your-api-key"));
}
```

### 4. Track Events

```cpp
if (auto* Framedash = GetGameInstance()->GetSubsystem<UFramedashSubsystem>())
{
    Framedash->Track(TEXT("player_death"), TEXT("Map01"), PlayerLocation);
}
```

### 5. Sampling

The global `SamplingRate` (project settings) applies to all `Player`-source
events; automatic events bypass sampling. High-frequency events can opt into
a lower per-event-name rate that overrides the global rate:

```cpp
if (auto* Framedash = GetGameInstance()->GetSubsystem<UFramedashSubsystem>())
{
    Framedash->SetEventSamplingRate(TEXT("ai_pathfind_step"), 0.05f); // ~5%
    Framedash->RemoveEventSamplingRate(TEXT("ai_pathfind_step"));      // back to global
}
```

`SetEventSamplingRate` and `RemoveEventSamplingRate` are also Blueprint-callable.

### 6. Automated profiling sessions (CI)

For build-over-build performance gating, tag a run's events with build metadata
so the dashboard and `framedash perf-diff` can compare one build against another.
Call this once after `InitializeTelemetry()` in your automated-test / profiling
entry point:

```cpp
if (auto* Framedash = GetGameInstance()->GetSubsystem<UFramedashSubsystem>())
{
    Framedash->BeginAutomatedSession(
        CommitSha,             // -> stamped as the first-class build_id field
        TEXT("main"),          // -> ci.branch attribute
        CommitSha,             // -> ci.commit attribute
        TEXT("boot_to_menu")); // -> ci.scenario attribute

    // ... run the scenario; gameplay + perf_heartbeat events are now tagged ...

    Framedash->Flush();
    Framedash->EndAutomatedSession();
}
```

`Branch`, `Commit`, and `Scenario` ride in the existing event `attributes` map
(`ci.*`), so nanopb is not regenerated; the tags apply to every event, including
the automatic `perf_heartbeat` that carries the frame-time / memory / GPU metrics.
A per-event attribute with the same key overrides the session value.

If your CI harness exports the standard Framedash variables (`FRAMEDASH_BUILD_ID`,
`FRAMEDASH_GIT_BRANCH`, `FRAMEDASH_GIT_COMMIT`, `FRAMEDASH_TEST_SCENARIO`) -- the
planned `framedash run-profile-test` runner will export these for you -- call the
no-argument `BeginAutomatedSessionFromEnvironment()` instead. All three functions
are Blueprint-callable. Then gate the build in CI with
`framedash perf-diff --baseline <old_build_id> --candidate <new_build_id> --fail-on-regression`.

Two things to know when wiring this into a real pipeline:

- `build_id` is the dimension `perf-diff` compares. It groups and compares by
  `build_id` (optionally narrowed by map/platform), not by `ci.scenario`, so two
  scenarios under one `build_id` fold into a single aggregate. To compare
  scenarios independently, give each its own `build_id` (for example
  `<commit>-<scenario>`) and treat `ci.scenario` as a queryable label rather than
  a `perf-diff` split key.
- The `ci.*` tags live in the event `attributes` map, which COPPA-redacted
  projects strip on ingest -- under COPPA only `build_id` survives. If you run
  automated profiling on a COPPA project, make `build_id` carry everything the
  comparison must distinguish.

## In-Editor Quickstart

Want to see a point on a real heatmap before wiring telemetry into your game?
**Activate your project from a Play-in-Editor (PIE) session** -- no packaged build
and no real players. Your project activates on its first real, map-qualified
spatial event; the automatic perf heartbeat sends an empty `map_id` and does
**not** count, so the quickstart sends one explicit `Track(EventName, MapId)` with
a non-empty, registered `map_id`.

1. **Register a map** in the dashboard (**Maps > Generate demo** or upload one) and
   copy a `map_id` -- the heatmap 404s on an unknown map.
2. **Get an Ingest key** (`events:write` scope; API keys > new key, **Ingest**
   preset).
3. From any actor's (or the Level Blueprint's) **Event BeginPlay**: **Get Framedash
   Subsystem** -> if **not** **Is Initialized**, **Initialize Telemetry** (`Api Key`)
   -> **Set Event Sampling Rate** (`quickstart_ping`, `1.0`) so a lowered global
   `SamplingRate` cannot drop the ping -> **Track** (`Event Name` = `quickstart_ping`,
   `Map Id` = your map, `Position` = a vector inside the map; the demo maps contain
   the origin) -> **Flush**.
4. Press **Play** (PIE). Open that map's heatmap -- your point appears in seconds.

> Blueprint logic is **not** stripped from packaged builds, so keep this in a
> throwaway test level (or remove it after activating) -- otherwise a forgotten
> `BeginPlay` graph would send `quickstart_ping` from real players in a shipped
> build.

The same flow as a copyable C++ actor (with **editor-only gating** -- guaranteed
inert in packaged builds -- plus field validation and the sampling override) ships
under [`Samples/InEditorQuickstart/`](Samples/InEditorQuickstart/README.md). These
are **real** events (not the dashboard "Generate demo" synthetic data), so they
count toward activation.

## Local Development

Step-by-step guide for testing the UE5 SDK against a local Framedash backend.

### Prerequisites

- Framedash monorepo cloned (e.g. `/path/to/framedash`)
- Docker Compose services running (`docker compose -f infra/docker-compose.yml up -d`)
  - PostgreSQL on `localhost:5432`, ClickHouse on `localhost:8123`
  - redis-http on `localhost:8079` (Upstash-compatible REST proxy)
- Node.js, pnpm installed

### 1. Create `.dev.vars` for Workers

The ingest and consumer Workers need secrets to access the database, ClickHouse, and Redis.

Start from the checked-in example files:

```bash
cp apps/ingest/.dev.vars.example apps/ingest/.dev.vars
cp apps/consumer/.dev.vars.example apps/consumer/.dev.vars
```

**`apps/ingest/.dev.vars`:**

```ini
DATABASE_URL=postgresql://framedash:framedash_dev@localhost:5432/framedash
CLICKHOUSE_URL=http://localhost:8123?user=default&password=framedash_dev
NEON_PROXY_PORT=4444
UPSTASH_REDIS_REST_URL=http://localhost:8079
UPSTASH_REDIS_REST_TOKEN=local_dev_token
```

**`apps/consumer/.dev.vars`:**

```ini
CLICKHOUSE_URL=http://localhost:8123?user=default&password=framedash_dev
UPSTASH_REDIS_REST_URL=http://localhost:8079
UPSTASH_REDIS_REST_TOKEN=local_dev_token
```

> `NEON_PROXY_PORT=4444` is required so the local ingest Worker can reach PostgreSQL through `infra/neon-proxy`. The Upstash Redis values point to the local `redis-http` Docker Compose service (Upstash-compatible REST proxy).

### 2. Start the Backend

```bash
# For SDK → ingest testing, run ingest standalone (avoids stale workerd issues with Turbo):
cd /path/to/framedash/apps/ingest
pnpm dev

# Or start all services via Turbo (web + ingest + consumer):
cd /path/to/framedash
pnpm dev
```

> **Warning**: `turbo dev` spawns wrangler/workerd processes that can linger after termination. For SDK testing, prefer running `apps/ingest` directly. If you used `turbo dev`, kill stale processes first: `taskkill /IM workerd.exe /F` (Windows) or `pkill workerd` (Unix). See `CLAUDE.md` for details.

When using `turbo dev`, all services start:

| Service | Port | Description |
|---------|------|-------------|
| Web dashboard | 3000 | `next dev` |
| Ingest Worker | 8787 | `wrangler dev` — receives SDK events |
| Consumer Worker | 8788 | `wrangler dev` — processes queue, writes to ClickHouse (production/queue path only; not required for local SDK testing with `wrangler.dev.toml`) |

### 3. Configure the UE5 Project

In your project's `Config/DefaultGame.ini`:

```ini
[/Script/Framedash.FramedashSettings]
ApiKey=your-api-key
bAutoInitialize=True
EndpointUrl="http://localhost:8787/v1/events"
```

> Quote the `EndpointUrl` value. An unquoted URL is truncated at `//` when UE reads the `.ini` (the value collapses to a bare `http:` / `https:`), which fails the endpoint check and silently drops every batch. UE strips the surrounding quotes on read, so the SDK sees the full URL.

> The API key must exist in your local PostgreSQL `api_keys` table. Create one via the web dashboard at `http://localhost:3000`.

### 4. Play in Editor

Hit **Play** (PIE) in the Unreal Editor. The SDK will:

1. Auto-initialize on `GameInstance` startup
2. Collect FPS, frame time, and memory metrics every tick
3. Buffer events and flush periodically (every ~30 seconds, 100 events, or 100KB — whichever first)
4. POST compressed Protobuf payloads to `http://localhost:8787/v1/events`

### 5. Verify Data

Check the wrangler dev terminal for request logs. Query ClickHouse to confirm events were written:

```bash
curl "http://localhost:8123/?database=framedash&query=SELECT+count()+FROM+events+FORMAT+Pretty"
```

Or with specific filters:

```bash
curl "http://localhost:8123/?database=framedash&query=SELECT+event_name,session_id,fps,frame_time_ms+FROM+events+ORDER+BY+timestamp+DESC+LIMIT+10+FORMAT+Pretty"
```

### Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `#include` compile error | Missing `Build.cs` dependency | Add `PrivateDependencyModuleNames.Add("Framedash")` |
| `SDK not initialized` log | Missing `bAutoInitialize=True` or empty `ApiKey` | Check `DefaultGame.ini` settings |
| 500 from ingest | Missing `.dev.vars` or services not running | Ensure PostgreSQL, ClickHouse, Redis are reachable |
| No data in ClickHouse | ClickHouse credentials wrong, or stale workerd hitting old code | Verify `CLICKHOUSE_URL` in `.dev.vars`; kill stale workerd processes (see Warning above) |
| Wrong port | Turbo started workers in different order | Ports are fixed: ingest=8787, consumer=8788 |

### Switching Back to Production

Delete the `EndpointUrl` line from `DefaultGame.ini` entirely. The SDK's compiled default is already `https://ingest.framedash.dev/v1/events`, so omitting the line is the cleanest way to point at production. Do not leave an empty `EndpointUrl=` (or blank the field in Project Settings): auto-init passes the configured value through as-is, so an empty value overrides the default with an empty endpoint that fails the check and drops every batch.

To override the endpoint (for example a self-hosted ingest), keep the value quoted so UE does not truncate it at `//`:

```ini
EndpointUrl="https://ingest.example.com/v1/events"
```
