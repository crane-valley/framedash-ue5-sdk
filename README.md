# Framedash UE5 SDK

Unreal Engine 5 plugin for collecting game telemetry and sending it to the Framedash platform.

## Requirements

- Unreal Engine 5.3 or newer. This is a source plugin that compiles against the Unreal Engine version of your project. The descriptor does not pin `EngineVersion`, so the plugin is not restricted to a single engine release.

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
EndpointUrl=http://localhost:8787/v1/events
```

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

Remove or clear the `EndpointUrl` line from `DefaultGame.ini`. The SDK defaults to `https://ingest.framedash.dev/v1/events`.
