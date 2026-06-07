#include "FramedashProtobufSerializer.h"

extern "C"
{
#include <pb_decode.h>
#include "telemetry.pb.h"
}

#include <gtest/gtest.h>

namespace
{

struct FStringDecodeContext
{
	std::string* Out;
};

struct FEventsDecodeContext
{
	std::vector<Framedash::FTelemetryEvent>* Events;
};

struct FStringMapDecodeContext
{
	std::vector<std::pair<std::string, std::string>>* Pairs;
};

struct FDoubleMapDecodeContext
{
	std::vector<std::pair<std::string, double>>* Pairs;
};

bool DecodeStringCb(pb_istream_t* Stream, const pb_field_t* Field, void** Arg)
{
	(void)Field;

	FStringDecodeContext* Ctx = static_cast<FStringDecodeContext*>(*Arg);
	if (!Ctx || !Ctx->Out) return false;

	Ctx->Out->resize(Stream->bytes_left);
	return pb_read(
		Stream,
		reinterpret_cast<pb_byte_t*>(Ctx->Out->data()),
		Ctx->Out->size());
}

bool DecodeAttributesCb(pb_istream_t* Stream, const pb_field_t* Field, void** Arg)
{
	(void)Field;

	FStringMapDecodeContext* Ctx = static_cast<FStringMapDecodeContext*>(*Arg);
	if (!Ctx || !Ctx->Pairs) return false;

	std::string Key;
	std::string Value;
	FStringDecodeContext KeyCtx{&Key};
	FStringDecodeContext ValueCtx{&Value};

	framedash_v1_GameTelemetryEvent_AttributesEntry Entry =
		framedash_v1_GameTelemetryEvent_AttributesEntry_init_zero;
	Entry.key.funcs.decode = &DecodeStringCb;
	Entry.key.arg = &KeyCtx;
	Entry.value.funcs.decode = &DecodeStringCb;
	Entry.value.arg = &ValueCtx;

	if (!pb_decode(Stream, framedash_v1_GameTelemetryEvent_AttributesEntry_fields, &Entry))
	{
		return false;
	}

	Ctx->Pairs->emplace_back(std::move(Key), std::move(Value));
	return true;
}

bool DecodeMetricsCb(pb_istream_t* Stream, const pb_field_t* Field, void** Arg)
{
	(void)Field;

	FDoubleMapDecodeContext* Ctx = static_cast<FDoubleMapDecodeContext*>(*Arg);
	if (!Ctx || !Ctx->Pairs) return false;

	std::string Key;
	FStringDecodeContext KeyCtx{&Key};

	framedash_v1_GameTelemetryEvent_MetricsEntry Entry =
		framedash_v1_GameTelemetryEvent_MetricsEntry_init_zero;
	Entry.key.funcs.decode = &DecodeStringCb;
	Entry.key.arg = &KeyCtx;

	if (!pb_decode(Stream, framedash_v1_GameTelemetryEvent_MetricsEntry_fields, &Entry))
	{
		return false;
	}

	Ctx->Pairs->emplace_back(std::move(Key), Entry.value);
	return true;
}

bool DecodeEventsCb(pb_istream_t* Stream, const pb_field_t* Field, void** Arg)
{
	(void)Field;

	FEventsDecodeContext* Ctx = static_cast<FEventsDecodeContext*>(*Arg);
	if (!Ctx || !Ctx->Events) return false;

	Framedash::FTelemetryEvent Event;
	FStringDecodeContext EventNameCtx{&Event.EventName};
	FStringDecodeContext SessionIdCtx{&Event.SessionId};
	FStringDecodeContext PlayerIdCtx{&Event.PlayerId};
	FStringDecodeContext MapIdCtx{&Event.MapId};
	FStringDecodeContext BuildIdCtx{&Event.BuildId};
	FStringDecodeContext PlatformCtx{&Event.Platform};
	FStringDecodeContext EngineVersionCtx{&Event.EngineVersion};
	FStringMapDecodeContext AttributesCtx{&Event.Attributes};
	FDoubleMapDecodeContext MetricsCtx{&Event.Metrics};

	framedash_v1_GameTelemetryEvent PbEvent = framedash_v1_GameTelemetryEvent_init_zero;
	PbEvent.event_name.funcs.decode = &DecodeStringCb;
	PbEvent.event_name.arg = &EventNameCtx;
	PbEvent.session_id.funcs.decode = &DecodeStringCb;
	PbEvent.session_id.arg = &SessionIdCtx;
	PbEvent.player_id.funcs.decode = &DecodeStringCb;
	PbEvent.player_id.arg = &PlayerIdCtx;
	PbEvent.map_id.funcs.decode = &DecodeStringCb;
	PbEvent.map_id.arg = &MapIdCtx;
	PbEvent.attributes.funcs.decode = &DecodeAttributesCb;
	PbEvent.attributes.arg = &AttributesCtx;
	PbEvent.metrics.funcs.decode = &DecodeMetricsCb;
	PbEvent.metrics.arg = &MetricsCtx;
	PbEvent.build_id.funcs.decode = &DecodeStringCb;
	PbEvent.build_id.arg = &BuildIdCtx;
	PbEvent.platform.funcs.decode = &DecodeStringCb;
	PbEvent.platform.arg = &PlatformCtx;
	PbEvent.engine_version.funcs.decode = &DecodeStringCb;
	PbEvent.engine_version.arg = &EngineVersionCtx;

	if (!pb_decode(Stream, framedash_v1_GameTelemetryEvent_fields, &PbEvent))
	{
		return false;
	}

	Event.TimestampUs = PbEvent.timestamp_us;
	if (PbEvent.has_position)
	{
		Event.Position = Framedash::FVector3{
			PbEvent.position.x,
			PbEvent.position.y,
			PbEvent.position.z,
		};
	}
	Event.Fps = PbEvent.fps;
	Event.FrameTimeMs = PbEvent.frame_time_ms;
	Event.MemoryUsedBytes = PbEvent.memory_used_bytes;
	Event.GpuTimeMs = PbEvent.gpu_time_ms;
	Event.Source = static_cast<Framedash::ETelemetrySource>(PbEvent.source);
	if (PbEvent.has_camera_yaw)
	{
		Event.CameraYaw = PbEvent.camera_yaw;
	}
	if (PbEvent.has_camera_pitch)
	{
		Event.CameraPitch = PbEvent.camera_pitch;
	}
	Event.GameThreadMs = PbEvent.game_thread_ms;
	Event.RenderThreadMs = PbEvent.render_thread_ms;

	Ctx->Events->push_back(std::move(Event));
	return true;
}

std::vector<Framedash::FTelemetryEvent> DecodeTelemetryBatch(const std::vector<uint8_t>& Bytes)
{
	std::vector<Framedash::FTelemetryEvent> Events;
	FEventsDecodeContext EventsCtx{&Events};

	framedash_v1_TelemetryBatch Batch = framedash_v1_TelemetryBatch_init_zero;
	Batch.events.funcs.decode = &DecodeEventsCb;
	Batch.events.arg = &EventsCtx;

	pb_istream_t Stream = pb_istream_from_buffer(Bytes.data(), Bytes.size());
	EXPECT_TRUE(pb_decode(&Stream, framedash_v1_TelemetryBatch_fields, &Batch));

	return Events;
}

} // anonymous namespace

TEST(ProtobufSerializer, SerializeTelemetryBatch_RoundTripsAllSupportedFields)
{
	Framedash::FTelemetryEvent Input;
	Input.EventName = "combat.hit";
	Input.TimestampUs = 1'773'000'000'123'456;
	Input.SessionId = "session-1";
	Input.PlayerId = "player-1";
	Input.Position = Framedash::FVector3{10.5f, -2.25f, 99.0f};
	Input.MapId = "arena";
	Input.Fps = 59.5f;
	Input.FrameTimeMs = 16.7f;
	Input.MemoryUsedBytes = 1'234'567'890;
	Input.GpuTimeMs = 7.25f;
	Input.Attributes = {{"weapon", "hammer"}, {"zone", "north"}};
	Input.Metrics = {{"damage", 42.5}, {"combo", 3.0}};
	Input.Source = Framedash::ETelemetrySource::Player;
	Input.BuildId = "build-2026.05.03";
	Input.Platform = "Windows";
	Input.EngineVersion = "5.4.4";
	Input.CameraYaw = 180.0f;
	Input.CameraPitch = -15.0f;
	Input.GameThreadMs = 5.5f;
	Input.RenderThreadMs = 6.5f;

	std::vector<uint8_t> Bytes;
	ASSERT_TRUE(Framedash::SerializeTelemetryBatch({Input}, Bytes));
	ASSERT_FALSE(Bytes.empty());

	const std::vector<Framedash::FTelemetryEvent> Output = DecodeTelemetryBatch(Bytes);
	ASSERT_EQ(Output.size(), 1u);

	const Framedash::FTelemetryEvent& Decoded = Output[0];
	EXPECT_EQ(Decoded.EventName, Input.EventName);
	EXPECT_EQ(Decoded.TimestampUs, Input.TimestampUs);
	EXPECT_EQ(Decoded.SessionId, Input.SessionId);
	EXPECT_EQ(Decoded.PlayerId, Input.PlayerId);
	ASSERT_TRUE(Decoded.Position.has_value());
	EXPECT_FLOAT_EQ(Decoded.Position->X, Input.Position->X);
	EXPECT_FLOAT_EQ(Decoded.Position->Y, Input.Position->Y);
	EXPECT_FLOAT_EQ(Decoded.Position->Z, Input.Position->Z);
	EXPECT_EQ(Decoded.MapId, Input.MapId);
	EXPECT_FLOAT_EQ(Decoded.Fps, Input.Fps);
	EXPECT_FLOAT_EQ(Decoded.FrameTimeMs, Input.FrameTimeMs);
	EXPECT_EQ(Decoded.MemoryUsedBytes, Input.MemoryUsedBytes);
	EXPECT_FLOAT_EQ(Decoded.GpuTimeMs, Input.GpuTimeMs);
	EXPECT_EQ(Decoded.Attributes, Input.Attributes);
	EXPECT_EQ(Decoded.Metrics, Input.Metrics);
	EXPECT_EQ(Decoded.Source, Input.Source);
	EXPECT_EQ(Decoded.BuildId, Input.BuildId);
	EXPECT_EQ(Decoded.Platform, Input.Platform);
	EXPECT_EQ(Decoded.EngineVersion, Input.EngineVersion);
	ASSERT_TRUE(Decoded.CameraYaw.has_value());
	ASSERT_TRUE(Decoded.CameraPitch.has_value());
	EXPECT_FLOAT_EQ(*Decoded.CameraYaw, *Input.CameraYaw);
	EXPECT_FLOAT_EQ(*Decoded.CameraPitch, *Input.CameraPitch);
	EXPECT_FLOAT_EQ(Decoded.GameThreadMs, Input.GameThreadMs);
	EXPECT_FLOAT_EQ(Decoded.RenderThreadMs, Input.RenderThreadMs);
}

TEST(ProtobufSerializer, SerializeTelemetryBatch_OmitsUnsetOptionalFields)
{
	Framedash::FTelemetryEvent Input;
	Input.EventName = "session_start";
	Input.TimestampUs = 1;
	Input.Source = Framedash::ETelemetrySource::Automated;

	std::vector<uint8_t> Bytes;
	ASSERT_TRUE(Framedash::SerializeTelemetryBatch({Input}, Bytes));

	const std::vector<Framedash::FTelemetryEvent> Output = DecodeTelemetryBatch(Bytes);
	ASSERT_EQ(Output.size(), 1u);

	EXPECT_FALSE(Output[0].Position.has_value());
	EXPECT_FALSE(Output[0].CameraYaw.has_value());
	EXPECT_FALSE(Output[0].CameraPitch.has_value());
	EXPECT_EQ(Output[0].Source, Framedash::ETelemetrySource::Automated);
}

TEST(ProtobufSerializer, SerializeTelemetryBatch_EmptyBatch_EncodesSuccessfully)
{
	std::vector<uint8_t> Bytes;
	ASSERT_TRUE(Framedash::SerializeTelemetryBatch({}, Bytes));

	const std::vector<Framedash::FTelemetryEvent> Output = DecodeTelemetryBatch(Bytes);
	EXPECT_TRUE(Output.empty());
}
