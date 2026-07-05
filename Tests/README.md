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

## What this does NOT cover

- The full UE5 plugin compile (verified separately by the `pnpm build`
  pipeline once a UE5 CI matrix lands -- see PLANS.md Phase 1.5)
- Anything that needs `FString`, `TArray`, HTTP module, etc. -- those
  classes still rely on UnrealEditor automation tests when added

The contract is: anything in `Source/Framedash/Private/Framedash*Policy.{h,cpp}`,
`FramedashProtobufSerializer.{h,cpp}`, and `FramedashUuid.{h,cpp}` must stay
engine-independent so this harness can keep growing.
