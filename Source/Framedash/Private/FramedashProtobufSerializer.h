// Copyright 2026 Crane Valley. All Rights Reserved.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Framedash
{

enum class ETelemetrySource : uint8_t
{
	Unspecified = 0,
	Player = 1,
	Automated = 2,
};

struct FVector3
{
	float X = 0.0f;
	float Y = 0.0f;
	float Z = 0.0f;
};

struct FTelemetryEvent
{
	std::string EventName;
	int64_t TimestampUs = 0;
	std::string SessionId;
	std::string PlayerId;
	std::optional<FVector3> Position;
	std::string MapId;
	float Fps = 0.0f;
	float FrameTimeMs = 0.0f;
	int64_t MemoryUsedBytes = 0;
	float GpuTimeMs = 0.0f;
	std::vector<std::pair<std::string, std::string>> Attributes;
	std::vector<std::pair<std::string, double>> Metrics;
	ETelemetrySource Source = ETelemetrySource::Player;
	std::string BuildId;
	std::string Platform;
	std::string EngineVersion;
	std::optional<float> CameraYaw;
	std::optional<float> CameraPitch;
	float GameThreadMs = 0.0f;
	float RenderThreadMs = 0.0f;
};

bool SerializeTelemetryBatch(
	const std::vector<FTelemetryEvent>& Events,
	std::vector<uint8_t>& OutBytes);

} // namespace Framedash
