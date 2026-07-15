# UE5 SDK -- Standalone Unit Tests

Engine-independent unit tests for the Framedash UE5 SDK policy classes.
These tests do NOT require an UnrealEditor or UE5 build -- they exercise
pure C++ classes extracted from the plugin (`FramedashRetryPolicy`, etc.)
so retry/flush logic can be regressed without spinning up the engine.

## What is covered

- `FramedashRetryPolicy` -- HTTP response classification, exponential
  backoff, batch-split decision. Based on the Unity retry tests, with
  UE5-specific assertions for preserved transport behavior: 3xx fail-fast and
  valid zero retry/delay configuration.
- `FramedashProtobufSerializer` -- nanopb batch serialization round-trip
  coverage for event fields, maps, and optional fields.
- `FramedashFlushPolicy` -- batch-size, payload-size, and interval triggers.
- `FramedashUuid` -- RFC 9562 UUIDv7 bit layout (`PackUuidV7`) and the
  Xoshiro256++ PRNG used to source rand_a / rand_b at session start.
- `FramedashAddressPlanner` -- prefer-IPv4-with-IPv6-fallback endpoint
  qualification, IPv4-first attempt ordering, family toggle, and the
  Host-header / request-target / port extraction the direct-socket fallback
  feeds into the raw request head.
- `FramedashRawHttp` -- raw HTTP/1.1 request-head builder, header/target
  sanitization (request-smuggling hygiene), and incremental status-line
  parsing. (The socket/TLS glue itself, `FramedashDirectSocketSender`, is
  UE-coupled and engine-only -- not host-testable by design.)

## Requirements

- CMake 3.20+
- A C++17 toolchain (MSVC 2022+, clang 14+, or gcc 11+)
- Internet access on first configure (CMake `FetchContent` downloads
  GoogleTest v1.15.2 into the build tree)

## Build and run on Windows (MSVC)

Run from a Developer Command Prompt for VS so `cmake` and the matching
generator are on PATH, then pick the generator string that matches your
VS install (`Visual Studio 17 2022` for VS 2022, `Visual Studio 18 2026`
for VS 2026, etc.):

```powershell
cmake -S sdks/ue5/Tests -B sdks/ue5/Tests/build -G "Visual Studio 17 2022" -A x64
cmake --build sdks/ue5/Tests/build --config Debug
ctest --test-dir sdks/ue5/Tests/build -C Debug --output-on-failure
```

If you need to invoke the VS-bundled CMake without opening a Developer
Prompt, derive the path with `vswhere`:

```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S sdks/ue5/Tests -B sdks/ue5/Tests/build -G "Visual Studio 17 2022" -A x64
```

## Build and run on Linux/macOS

```bash
cmake -S sdks/ue5/Tests -B sdks/ue5/Tests/build
cmake --build sdks/ue5/Tests/build
ctest --test-dir sdks/ue5/Tests/build --output-on-failure
```

## CI

This harness runs on every UE5 SDK change in `.github/workflows/ue5-ci.yml`
(the `googletest` job) on the self-hosted Windows runner, using the
VS-bundled CMake resolved via `vswhere` and the `Visual Studio 18 2026`
generator (Release config). The same workflow also runs a RunUAT
`BuildPlugin` matrix that compiles the full plugin against installed engines,
and an `automation-spec` job that runs the in-engine Automation Specs (below)
headless on UE 5.6.

## In-engine Automation Specs

Runtime behavior that needs a live engine (`FString`/`TArray`, the HTTP module,
the file-backed offline queue) is covered by UE Automation Specs under
`Source/Framedash/Private/Tests/`. They are gated behind BOTH
`WITH_DEV_AUTOMATION_TESTS` and `FRAMEDASH_WITH_AUTOMATION_SPECS`, and the latter is
defined only when `Framedash.Build.cs` sees the environment variable
`FRAMEDASH_BUILD_AUTOMATION_SPECS=1`. This keeps the specs out of every normal build
(including the RunUAT `BuildPlugin` redistributable, whose Development DLLs would
otherwise carry them): their setup destructively clears the project's
`Saved/Framedash` offline queue, so they must never compile into a shipped binary.

- `FramedashShutdownSpec` -- the `UFramedashSubsystem` shutdown/durability
  contract: events still buffered at teardown are persisted; an in-flight batch
  that was flushed but never delivered is persisted; a teardown with the offline
  queue disabled drops events without crashing (fail-safe); a fresh subsystem
  restores persisted events on initialize.

The `automation-spec` CI job assembles a throwaway host project, copies the plugin
in, builds the host editor modules with UBT (the editor does not auto-compile a
code plugin under `-unattended`), then runs
`UnrealEditor-Cmd FramedashHost.uproject -ExecCmds="Automation RunTests Framedash; Quit"
-unattended -nullrhi -nosplash -ReportExportPath=<dir>` and fails the step by
parsing the exported `index.json` (the editor exit code is not a reliable pass/fail
signal). To run locally, assemble the same host under a short path (e.g.
`C:\uet\fd-spec-host` -- UE hits MAX_PATH on deep Intermediate trees) with the
committed `Tests/HostProject/FramedashHost.uproject`, set
`FRAMEDASH_BUILD_AUTOMATION_SPECS=1` before the UBT build (so the specs compile in),
then build the host editor modules and run `UnrealEditor-Cmd` as above.

## What this does NOT cover

- The full UE5 plugin compile (covered by the RunUAT `BuildPlugin` matrix in
  `.github/workflows/ue5-ci.yml`, not by this harness)
- Runtime behavior of anything that needs `FString`, `TArray`, HTTP module,
  etc. -- those classes are covered by the in-engine Automation Specs (see the
  "In-engine Automation Specs" section above), not by this harness.

The contract is: anything in `Source/Framedash/Private/Framedash*Policy.{h,cpp}`,
`FramedashProtobufSerializer.{h,cpp}`, and `FramedashUuid.{h,cpp}` must stay
engine-independent so this harness can keep growing.
