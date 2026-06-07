# Changelog

All notable changes to the Framedash UE5 SDK are documented here. This project
follows [Keep a Changelog](https://keepachangelog.com/) and
[Semantic Versioning](https://semver.org/).

## [0.1.0] - 2026-06-06

Initial public pre-release (beta).

- Unreal Engine 5 telemetry plugin via the Game Instance Subsystem
  (`UFramedashSubsystem::InitializeTelemetry` and `Track`).
- Automatic performance heartbeats (frame time, GPU time, game/render thread,
  memory).
- Batched Protobuf transport (nanopb) with retry and a disk-backed offline queue.
- Source plugin supporting Unreal Engine 5.3 and newer.
