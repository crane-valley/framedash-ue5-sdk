# Changelog

All notable changes to the Framedash UE5 SDK are documented here. This project
follows [Keep a Changelog](https://keepachangelog.com/) and
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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
