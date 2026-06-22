// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.generated.h"

/** Source of the telemetry event. */
UENUM(BlueprintType)
enum class EFramedashTelemetrySource : uint8
{
	Unspecified = 0,
	Player = 1,
	Automated = 2,
};

/** Internal event representation before Protobuf serialization. */
struct FFramedashEvent
{
	FString EventName;
	int64 TimestampUs = 0;
	FString SessionId;
	FString PlayerId;
	FVector Position = FVector::ZeroVector;
	FString MapId;
	float Fps = 0.0f;
	float FrameTimeMs = 0.0f;
	int64 MemoryUsedBytes = 0;
	float GpuTimeMs = 0.0f;
	TMap<FString, FString> Attributes;
	TMap<FString, double> Metrics;
	EFramedashTelemetrySource Source = EFramedashTelemetrySource::Player;
	FString BuildId;
	FString Platform;
	FString EngineVersion;
	TOptional<float> CameraYaw;
	TOptional<float> CameraPitch;
	float GameThreadMs = 0.0f;
	float RenderThreadMs = 0.0f;
};

/** Performance metrics snapshot. */
struct FFramedashPerformanceSnapshot
{
	float Fps = 0.0f;
	float FrameTimeMs = 0.0f;
	int64 MemoryUsedBytes = 0;
	float GpuTimeMs = 0.0f;
	float GameThreadMs = 0.0f;
	float RenderThreadMs = 0.0f;
};

/** SDK constants (matching Unity SDK). */
namespace FramedashConstants
{
	constexpr int32 MaxBatchSize = 100;
	constexpr float FlushIntervalSeconds = 30.0f;
	constexpr float HeartbeatIntervalSeconds = 10.0f;
	constexpr int32 MaxPayloadBytes = 102400; // 100 KB
	constexpr int32 MaxRetries = 5;
	constexpr float HttpTimeoutSeconds = 30.0f;
	constexpr int32 MaxEventNameLength = 128;
	constexpr int32 MaxAttributePairs = 50;
	constexpr int32 MaxMetricPairs = 50;
	// Per-field client-side string-length caps mirroring the ingest server limits
	// (packages/ingest-core/src/config.ts). Ingest validation drops the WHOLE
	// batch when any single event field exceeds a limit, so the SDK truncates each
	// string field before enqueue via TruncateToUtf16Units (FramedashStringUtil.h),
	// which counts UTF-16 code units -- the server's JS string-length unit -- on
	// both 2- and 4-byte TCHAR builds. The numeric clamps (position / fps / timing
	// / memory) live in FramedashFieldClamps.h, which is kept Unreal-header-free so
	// it can be unit-tested without an editor build.
	constexpr int32 MaxMapIdLength = 128;
	constexpr int32 MaxBuildIdLength = 128;
	constexpr int32 MaxPlayerIdLength = 128;
	constexpr int32 MaxAttributeKeyLength = 64;
	constexpr int32 MaxAttributeValueLength = 512;
	// Metric keys share the attribute-key cap server-side (validation.ts checks
	// metric keys against MAX_ATTRIBUTE_KEY_LEN too); alias rather than duplicate
	// the literal so the two cannot drift while the metric call site stays readable.
	constexpr int32 MaxMetricKeyLength = MaxAttributeKeyLength;
	constexpr int32 MaxPlatformLength = 64;
	constexpr int32 MaxEngineVersionLength = 64;
	constexpr int32 MaxPersistedEvents = 1000;
	constexpr int32 EventBufferCapacity = MaxPersistedEvents + MaxBatchSize;
	constexpr int32 EstimatedBytesPerEvent = 500;
}

// String constants outside namespace to avoid UHT parsing issues with inline variables
#define FRAMEDASH_SDK_VERSION TEXT("0.1.1")
#define FRAMEDASH_SDK_NAME TEXT("ue5")
