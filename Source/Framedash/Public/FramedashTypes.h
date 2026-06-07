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
	constexpr int32 MaxPersistedEvents = 1000;
	constexpr int32 EventBufferCapacity = MaxPersistedEvents + MaxBatchSize;
	constexpr int32 EstimatedBytesPerEvent = 500;
}

// String constants outside namespace to avoid UHT parsing issues with inline variables
#define FRAMEDASH_SDK_VERSION TEXT("0.1.0")
#define FRAMEDASH_SDK_NAME TEXT("ue5")
