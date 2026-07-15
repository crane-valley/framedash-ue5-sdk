# Changelog

All notable changes to the Framedash UE5 SDK are documented here. This project
follows [Keep a Changelog](https://keepachangelog.com/) and
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.8] - 2026-07-15

### Fixed

- Fab technical-review resubmission after the 0.1.7 package was rejected:
  - Every shipped C, C++, header, and Build.cs file now starts with the publisher
    name and 2026 publication year.
  - Vendored nanopb moved from the runtime module's private directory to the
    required plugin-level `Source/ThirdParty/nanopb/` directory; build and test
    include paths follow the new location.
  - `Config/FilterPlugin.ini` now includes itself, and the packaging script
    requires it in BuildPlugin output so a submission cannot silently omit the
    manifest.
  - Fab-flavor zips omit Framedash's first-party MIT `LICENSE` while retaining
    nanopb's zlib license and third-party notice. GitHub/source distribution
    remains MIT-licensed.
- The final Fab zip now passes `scripts/test-ue5-fab-package.ps1` before the
  release script accepts it. The gate checks licensing, copyright headers,
  third-party placement, filter coverage, descriptor metadata, forbidden build
  folders, and Fab's 170-character path limit against the actual archive.

## [0.1.7] - 2026-07-15

### Changed

- Fab technical-review resubmission. Epic Fab rejected the 0.1.6 package for
  three descriptor/licensing issues, now fixed. The release format is now one
  zip per engine version (5.3-5.8), built by `scripts/package-ue5-plugin.ps1`
  and, on a `sdk-ue5-v*` tag, by the new `ue5-release.yml` CI workflow:
  - Per-package descriptor pinning. `-TargetEngineVersion` (5.3-5.8) stamps
    `"EngineVersion": "<ver>.0"` and each module's `PlatformAllowList` (Framedash
    runtime: Win64 + Android; FramedashEditor: Win64) into the PACKAGED
    descriptor only -- the tracked source `Framedash.uplugin` stays unpinned and
    unrestricted so the public source mirror still builds on every platform
    `Framedash.Build.cs` supports (Win64/Mac/iOS/Android/Unix).
  - Two distribution flavors via `-Channel`: `github` ships compiled Win64
    Binaries (no rebuild prompt for Launcher users); `fab` is source-only
    (Binaries stripped, per Epic's Fab upload checklist).
  - Copyright headers added to every shipped code file: a Crane Valley banner on
    the generated nanopb `telemetry.pb.c/.h` (emitted by the generator so
    regenerated output stays byte-fresh), and a Crane Valley + upstream nanopb
    (Petteri Aimonen, zlib) attribution block on the vendored nanopb runtime
    (`pb*.c/.h`). Replaced a stray Epic Games template header in `Framedash.h`.

## [0.1.6] - 2026-07-12

### Added

- Map/level load-time capture: `BeginMapLoad(MapName)` / `EndMapLoad()`
  Blueprint-callable functions time a load on `FPlatformTime::Seconds`
  (monotonic real wall time, unaffected by pause / time dilation), and
  `ReportMapLoad(MapName, LoadTimeMs)` lets a custom loader report a load time
  directly. Both paths emit a `map_load` auto event carrying
  `metrics["load_time_ms"]` and `attributes["map_name"]`; `map_id` is
  deliberately left empty so the event stays out of the spatial heatmap grid
  and the activation gate. `ReportMapLoad` drops (does not clamp) a NaN,
  Infinity, or negative `LoadTimeMs`. Calling `BeginMapLoad` again before
  `EndMapLoad` replaces the pending measurement. Game-thread only; fail-safe
  (never throws, no-op if the SDK is not initialized).
- `io.*` disk metrics: a new opt-in `bTrackDiskIo` project setting (Project
  Settings > Framedash, default OFF) chains an `IPlatformFile` wrapper at init
  that counts synchronous disk-read bytes/time/ops and attaches them as
  `io.read_bytes` / `io.read_time_ms` / `io.read_ops` (deltas since the
  previous heartbeat) on `perf_heartbeat`. Wrapping the platform file layer is
  invasive, so it stays opt-in; the IoDispatcher/IoStore path (zen loader,
  Nanite streaming) bypasses `IPlatformFile`, so raw counters undercount
  Nanite-heavy IO. `ReportIoSample(Bytes, ReadTimeMs, Ops)` is
  Blueprint-callable and works regardless of the `bTrackDiskIo` setting.

## [0.1.5] - 2026-07-05

### Added

- Prefer-IPv4-with-IPv6-fallback ingest connect via a direct-socket TLS
  fallback (parity with the Unity SDK fix). On a transport-level failure
  (status 0) of the primary `IHttpRequest` attempt -- which cannot pin an
  address family (engine libcurl, no portable `CURLOPT_IPRESOLVE`) -- the
  transport resolves the endpoint to concrete IPv4/IPv6 addresses on a
  background thread and retries within the same attempt over an `FSocket`
  connected to the resolved IPv4 literal (IPv6 fallback; the family toggles
  only on repeated transport-level failure), with TLS via the engine SSL
  module's OpenSSL: full standard chain/expiry/hostname validation against
  the original FQDN (`SSL_VERIFY_PEER` + `SSL_set1_host` + SNI, TLS 1.2
  floor, engine trust roots) and `Host: <fqdn>` so Cloudflare routing is
  unchanged. Broken-IPv6 hosts (blackholed AAAA route) now deliver in-flush
  over IPv4 instead of parking every batch in the offline queue until the
  persisted cap. Retry accounting: `MaxRetries` bounds primary attempts;
  each transport-level primary failure may add one fallback POST (worst case
  2x POSTs and ~20s wall per attempt on total blackout). Endpoints whose
  ingest domain has pinned public keys, loopback/IP-literal/non-HTTPS
  endpoints, and platforms without the engine SSL module keep the previous
  `IHttpRequest`-only behavior unchanged.

## [0.1.4] - 2026-07-05

### Changed

- Transport whole-request timeout bounded 30s -> 10s. On a broken-IPv6 network
  (a global AAAA advertised via Router Advertisement with no working route)
  the connect attempt may still block up to the request timeout; IHttpRequest
  (libcurl) exposes no portable way to tune address-family behavior, so the
  shorter timeout fails fast instead of stalling 30s. The offline queue (on by
  default) keeps the timed-out batch so it is retried on the next
  run/initialization, once a run resolves a reachable IPv4. This is fail-fast,
  not an address-family fallback: a permanently-broken-IPv6 client that never
  yields IPv4 still bounds at the persisted-queue cap. Mirrors the Unity SDK
  fix (#1217); the full prefer-IPv4 transport (direct-socket path, like Unity
  #1218) is deferred -- IHttpRequest exposes no portable CURLOPT_IPRESOLVE.

## [0.1.3] - 2026-07-05

### Fixed

- Setup docs no longer silently drop all telemetry. An unquoted `EndpointUrl` in
  `Config/DefaultGame.ini` is truncated at `//` when UE reads the `.ini` (the
  value collapses to a bare `http:` / `https:`), which fails the endpoint check
  and drops every batch. The README now quotes the value (UE strips the quotes
  on read) and recommends omitting the line to use the compiled default; the SDK
  logs a targeted hint when it detects a truncated `http:` / `https:` endpoint.
- The redistributable release zip is no longer pinned to a single engine build.
  `RunUAT BuildPlugin` stamped the packaged descriptor with an `EngineVersion`
  equal to the build engine, which raised a blocking "designed for build X" modal
  on other 5.x versions and broke headless/CI starts; the packaging script now
  strips it so the plugin stays unpinned (UE 5.3+ floor unchanged).
- `DocsURL` now points at the live guide (`/en/sdk/unreal/`) instead of the
  dead `/en/sdk/ue5` URL.

## [0.1.2] - 2026-06-30

### Added

- Automated profiling sessions for CI: `BeginAutomatedSession(BuildId, Branch,
  Commit, Scenario)` (and `BeginAutomatedSessionFromEnvironment()`, which reads the
  `FRAMEDASH_BUILD_ID` / `FRAMEDASH_GIT_BRANCH` / `FRAMEDASH_GIT_COMMIT` /
  `FRAMEDASH_TEST_SCENARIO` environment variables) tag every subsequent event with
  CI metadata so build-over-build performance can be compared in the dashboard and
  via `framedash perf-diff`. The build id is stamped as the first-class `build_id`;
  branch, commit, and scenario ride in the existing attributes map as `ci.branch` /
  `ci.commit` / `ci.scenario`, so nanopb is not regenerated. `EndAutomatedSession()`
  stops the tagging. The session tags merge into every event -- including the
  automatic `perf_heartbeat` that carries the performance metrics -- and a per-event
  attribute with the same key overrides the session value. All three are
  Blueprint-callable.
- In-editor quickstart: activate a project from a Play-in-Editor session (no
  packaged build, no real players) by sending one explicit map-qualified
  `Track(EventName, MapId)`. A README "In-Editor Quickstart" section documents
  the Blueprint flow, and a copyable C++ sample actor ships under
  `Samples/InEditorQuickstart/` (staged into the redistributable but outside
  `Source/`, so UBT never compiles it). The actor's configuration fields are
  `WITH_EDITORONLY_DATA` and its activation logic is `WITH_EDITOR`, so both are
  stripped from packaged builds while the inert `UCLASS` shell remains. The actor
  validates the Map Id (always) and the Api Key (only when it must initialize)
  before sending, so a misconfigured actor emits no telemetry and the "already
  initialized" path works without re-entering a key; it forces a per-event
  sampling override of 1.0 so a lowered global `SamplingRate` cannot drop the
  activation ping. The README notes that the Blueprint path, unlike the C++ actor,
  is not stripped from packaged builds.

## [0.1.1] - 2026-06-22

### Changed

- Restore Unreal Engine 5.3+ build support. Several engine-API usages that only
  exist on newer releases had crept in and silently raised the compile floor to
  5.6 (each is fatal because `BuildPlugin` compiles with `-WarningsAsErrors`):
  - `Build.cs` used `CppCompileWarningSettings` (UE 5.6+); it now selects the
    per-release warning-suppression API (UE 5.3-5.4 / 5.5 / 5.6+).
  - The transport used the `UE_VERSION_NEWER_THAN_OR_EQUAL` macro (added in UE
    5.6); it now uses the equivalent `!UE_VERSION_OLDER_THAN`, defined on all 5.x.
  - The transport relied on `Containers/Ticker.h` being transitively included
    (only true on 5.6); it is now included explicitly.
  - `RemoveAt`/`RemoveAtSwap` passed `EAllowShrinking::No` (UE 5.4+); a small
    compat shim passes the pre-5.4 `bool` form on UE 5.3.

  Build-verified via RunUAT BuildPlugin (`-WarningsAsErrors`) on UE 5.3, 5.4,
  5.5, and 5.6.

### Security

- Detect a credential-leaking cross-origin redirect on the telemetry transport.
  The Unreal HTTP backend (libcurl) follows 3xx redirects and re-sends the
  `X-API-Key` header to the redirect target, and Unreal exposes no portable
  per-request toggle to disable following (unlike the Unity SDK's
  `redirectLimit = 0`). The transport now inspects each request's effective URL
  (on both success and transport failure) and, when a redirect crossed origin
  (scheme, host, or port, or a libcurl-differential `@`/`\` host), drops the
  batch without persisting or retrying and logs a security error instead of
  trusting the redirect target's response. This is defense in depth: it does not
  undo the one-time header transmission the redirect already performed, and -- as
  only the final effective URL is visible -- it cannot detect a chain that leaves
  and returns to the configured origin. The HTTPS-only endpoint policy (TLS
  authenticates the endpoint) keeps the realistic risk low. Detection requires
  `IHttpBase::GetEffectiveURL` (UE 5.4+); on UE 5.3 the transport keeps its prior
  behavior.

### Fixed

- Clamp every per-event field to the ingest server limits client-side
  (event name, map id, build id, player id, position, attributes, metrics, and
  all performance metrics). Ingest validation rejects the whole batch if any
  single field is out of range, so one over-limit field previously dropped every
  event in that flush. The caps now match the Godot SDK.
- Flush and heartbeat cadence no longer stalls while the game is paused or
  stretches under time dilation: the subsystem now ticks when paused and drives
  the 30s flush / 10s heartbeat timers from real wall-clock time instead of the
  engine-scaled delta.
- `SetPlayerId` called off the game thread no longer crashes via a fatal assert;
  it warns and ignores the call (the never-crash rule).

## [0.1.0] - 2026-06-06

Initial public pre-release (beta).

- Unreal Engine 5 telemetry plugin via the Game Instance Subsystem
  (`UFramedashSubsystem::InitializeTelemetry` and `Track`).
- Automatic performance heartbeats (frame time, GPU time, game/render thread,
  memory).
- Batched Protobuf transport (nanopb) with retry and a disk-backed offline queue.
- Source plugin supporting Unreal Engine 5.3 and newer.
