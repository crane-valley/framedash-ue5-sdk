// Copyright Crane Valley. All Rights Reserved.

#include "FramedashProtobufSerializer.h"

#include <algorithm>
#include <cstring>
#include <limits>

extern "C"
{
#include <pb_encode.h>
#include "telemetry.pb.h"
}

namespace
{

struct FStringEncodeContext
{
	const std::string* Str;
};

struct FEventsEncodeContext
{
	const std::vector<Framedash::FTelemetryEvent>* Events;
};

struct FStringMapEncodeContext
{
	const std::vector<std::pair<std::string, std::string>>* Pairs;
};

struct FDoubleMapEncodeContext
{
	const std::vector<std::pair<std::string, double>>* Pairs;
};

framedash_v1_TelemetrySource ToProtoSource(Framedash::ETelemetrySource Source)
{
	switch (Source)
	{
	case Framedash::ETelemetrySource::Player:
		return framedash_v1_TelemetrySource_TELEMETRY_SOURCE_PLAYER;
	case Framedash::ETelemetrySource::Automated:
		return framedash_v1_TelemetrySource_TELEMETRY_SOURCE_AUTOMATED;
	case Framedash::ETelemetrySource::Unspecified:
	default:
		return framedash_v1_TelemetrySource_TELEMETRY_SOURCE_UNSPECIFIED;
	}
}

bool EncodeStringCb(pb_ostream_t* Stream, const pb_field_t* Field, void* const* Arg)
{
	const FStringEncodeContext* Ctx = static_cast<const FStringEncodeContext*>(*Arg);
	if (!Ctx || !Ctx->Str || Ctx->Str->empty()) return true;

	if (!pb_encode_tag_for_field(Stream, Field)) return false;
	return pb_encode_string(
		Stream,
		reinterpret_cast<const pb_byte_t*>(Ctx->Str->data()),
		Ctx->Str->size());
}

bool EncodeAttributesCb(pb_ostream_t* Stream, const pb_field_t* Field, void* const* Arg)
{
	const FStringMapEncodeContext* Ctx = static_cast<const FStringMapEncodeContext*>(*Arg);
	if (!Ctx || !Ctx->Pairs) return true;

	for (const auto& Pair : *Ctx->Pairs)
	{
		framedash_v1_GameTelemetryEvent_AttributesEntry Entry =
			framedash_v1_GameTelemetryEvent_AttributesEntry_init_zero;

		FStringEncodeContext KeyCtx{&Pair.first};
		FStringEncodeContext ValCtx{&Pair.second};

		Entry.key.funcs.encode = &EncodeStringCb;
		Entry.key.arg = &KeyCtx;
		Entry.value.funcs.encode = &EncodeStringCb;
		Entry.value.arg = &ValCtx;

		if (!pb_encode_tag_for_field(Stream, Field)) return false;
		if (!pb_encode_submessage(
			Stream,
			framedash_v1_GameTelemetryEvent_AttributesEntry_fields,
			&Entry))
		{
			return false;
		}
	}

	return true;
}

bool EncodeMetricsCb(pb_ostream_t* Stream, const pb_field_t* Field, void* const* Arg)
{
	const FDoubleMapEncodeContext* Ctx = static_cast<const FDoubleMapEncodeContext*>(*Arg);
	if (!Ctx || !Ctx->Pairs) return true;

	for (const auto& Pair : *Ctx->Pairs)
	{
		framedash_v1_GameTelemetryEvent_MetricsEntry Entry =
			framedash_v1_GameTelemetryEvent_MetricsEntry_init_zero;

		FStringEncodeContext KeyCtx{&Pair.first};

		Entry.key.funcs.encode = &EncodeStringCb;
		Entry.key.arg = &KeyCtx;
		Entry.value = Pair.second;

		if (!pb_encode_tag_for_field(Stream, Field)) return false;
		if (!pb_encode_submessage(
			Stream,
			framedash_v1_GameTelemetryEvent_MetricsEntry_fields,
			&Entry))
		{
			return false;
		}
	}

	return true;
}

bool EncodeEventsCb(pb_ostream_t* Stream, const pb_field_t* Field, void* const* Arg)
{
	const FEventsEncodeContext* Ctx = static_cast<const FEventsEncodeContext*>(*Arg);
	if (!Ctx || !Ctx->Events) return true;

	for (const Framedash::FTelemetryEvent& Evt : *Ctx->Events)
	{
		framedash_v1_GameTelemetryEvent PbEvent = framedash_v1_GameTelemetryEvent_init_zero;

		FStringEncodeContext EventNameCtx{&Evt.EventName};
		PbEvent.event_name.funcs.encode = &EncodeStringCb;
		PbEvent.event_name.arg = &EventNameCtx;

		PbEvent.timestamp_us = Evt.TimestampUs;

		FStringEncodeContext SessionIdCtx{&Evt.SessionId};
		PbEvent.session_id.funcs.encode = &EncodeStringCb;
		PbEvent.session_id.arg = &SessionIdCtx;

		FStringEncodeContext PlayerIdCtx{&Evt.PlayerId};
		PbEvent.player_id.funcs.encode = &EncodeStringCb;
		PbEvent.player_id.arg = &PlayerIdCtx;

		if (Evt.Position.has_value())
		{
			PbEvent.has_position = true;
			PbEvent.position.x = Evt.Position->X;
			PbEvent.position.y = Evt.Position->Y;
			PbEvent.position.z = Evt.Position->Z;
		}

		FStringEncodeContext MapIdCtx{&Evt.MapId};
		PbEvent.map_id.funcs.encode = &EncodeStringCb;
		PbEvent.map_id.arg = &MapIdCtx;

		PbEvent.fps = Evt.Fps;
		PbEvent.frame_time_ms = Evt.FrameTimeMs;
		PbEvent.memory_used_bytes = Evt.MemoryUsedBytes;
		PbEvent.gpu_time_ms = Evt.GpuTimeMs;

		FStringMapEncodeContext AttrsCtx{&Evt.Attributes};
		PbEvent.attributes.funcs.encode = &EncodeAttributesCb;
		PbEvent.attributes.arg = &AttrsCtx;

		FDoubleMapEncodeContext MetricsCtx{&Evt.Metrics};
		PbEvent.metrics.funcs.encode = &EncodeMetricsCb;
		PbEvent.metrics.arg = &MetricsCtx;

		PbEvent.source = ToProtoSource(Evt.Source);

		FStringEncodeContext BuildIdCtx{&Evt.BuildId};
		PbEvent.build_id.funcs.encode = &EncodeStringCb;
		PbEvent.build_id.arg = &BuildIdCtx;

		FStringEncodeContext PlatformCtx{&Evt.Platform};
		PbEvent.platform.funcs.encode = &EncodeStringCb;
		PbEvent.platform.arg = &PlatformCtx;

		FStringEncodeContext EngineVersionCtx{&Evt.EngineVersion};
		PbEvent.engine_version.funcs.encode = &EncodeStringCb;
		PbEvent.engine_version.arg = &EngineVersionCtx;

		if (Evt.CameraYaw.has_value())
		{
			PbEvent.has_camera_yaw = true;
			PbEvent.camera_yaw = *Evt.CameraYaw;
		}
		if (Evt.CameraPitch.has_value())
		{
			PbEvent.has_camera_pitch = true;
			PbEvent.camera_pitch = *Evt.CameraPitch;
		}

		PbEvent.game_thread_ms = Evt.GameThreadMs;
		PbEvent.render_thread_ms = Evt.RenderThreadMs;

		if (!pb_encode_tag_for_field(Stream, Field)) return false;
		if (!pb_encode_submessage(Stream, framedash_v1_GameTelemetryEvent_fields, &PbEvent))
		{
			return false;
		}
	}

	return true;
}

bool ShouldRetryWithLargerBuffer(const pb_ostream_t& Stream)
{
	const char* Error = PB_GET_ERROR(&Stream);
	return (Error && std::strcmp(Error, "stream full") == 0)
		|| Stream.bytes_written == Stream.max_size;
}

} // anonymous namespace

namespace Framedash
{

bool SerializeTelemetryBatch(
	const std::vector<FTelemetryEvent>& Events,
	std::vector<uint8_t>& OutBytes)
{
	constexpr size_t MaxRetries = 3;
	if (Events.size() > std::numeric_limits<size_t>::max() / 1024)
	{
		OutBytes.clear();
		return false;
	}

	size_t BufferSize = std::max<size_t>(Events.size() * 1024, 4096);

	for (size_t Attempt = 0; Attempt < MaxRetries; ++Attempt)
	{
		OutBytes.resize(BufferSize);

		pb_ostream_t Stream = pb_ostream_from_buffer(OutBytes.data(), OutBytes.size());

		framedash_v1_TelemetryBatch Batch = framedash_v1_TelemetryBatch_init_zero;
		FEventsEncodeContext EventsCtx{&Events};
		Batch.events.funcs.encode = &EncodeEventsCb;
		Batch.events.arg = &EventsCtx;

		if (pb_encode(&Stream, framedash_v1_TelemetryBatch_fields, &Batch))
		{
			OutBytes.resize(Stream.bytes_written);
			return true;
		}

		if (!ShouldRetryWithLargerBuffer(Stream))
		{
			break;
		}

		if (BufferSize > std::numeric_limits<size_t>::max() / 2)
		{
			break;
		}

		BufferSize *= 2;
	}

	OutBytes.clear();
	return false;
}

} // namespace Framedash
