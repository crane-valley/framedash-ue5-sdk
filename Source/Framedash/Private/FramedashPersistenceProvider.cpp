// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashPersistenceProvider.h"
#include "Framedash.h"
#include "FramedashEngineCompat.h"
#include "FramedashIoStats.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
constexpr int32 PersistenceVersion = 1;
FCriticalSection QueueFileCriticalSection;

TSharedRef<FJsonObject> MapStringToJsonObject(const TMap<FString, FString>& Map)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Map)
	{
		Object->SetStringField(Pair.Key, Pair.Value);
	}
	return Object;
}

TSharedRef<FJsonObject> MapDoubleToJsonObject(const TMap<FString, double>& Map)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const TPair<FString, double>& Pair : Map)
	{
		Object->SetNumberField(Pair.Key, Pair.Value);
	}
	return Object;
}

TSharedRef<FJsonObject> EventToJsonObject(const FFramedashEvent& Event)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("eventName"), Event.EventName);
	Object->SetStringField(TEXT("timestampUs"), LexToString(Event.TimestampUs));
	Object->SetStringField(TEXT("sessionId"), Event.SessionId);
	Object->SetStringField(TEXT("playerId"), Event.PlayerId);

	TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), Event.Position.X);
	Position->SetNumberField(TEXT("y"), Event.Position.Y);
	Position->SetNumberField(TEXT("z"), Event.Position.Z);
	Object->SetObjectField(TEXT("position"), Position);

	Object->SetStringField(TEXT("mapId"), Event.MapId);
	Object->SetNumberField(TEXT("fps"), Event.Fps);
	Object->SetNumberField(TEXT("frameTimeMs"), Event.FrameTimeMs);
	Object->SetStringField(TEXT("memoryUsedBytes"), LexToString(Event.MemoryUsedBytes));
	Object->SetNumberField(TEXT("gpuTimeMs"), Event.GpuTimeMs);
	Object->SetObjectField(TEXT("attributes"), MapStringToJsonObject(Event.Attributes));
	Object->SetObjectField(TEXT("metrics"), MapDoubleToJsonObject(Event.Metrics));
	Object->SetNumberField(TEXT("source"), static_cast<int32>(Event.Source));
	Object->SetStringField(TEXT("buildId"), Event.BuildId);
	Object->SetStringField(TEXT("platform"), Event.Platform);
	Object->SetStringField(TEXT("engineVersion"), Event.EngineVersion);

	if (Event.CameraYaw.IsSet())
	{
		Object->SetNumberField(TEXT("cameraYaw"), Event.CameraYaw.GetValue());
	}
	if (Event.CameraPitch.IsSet())
	{
		Object->SetNumberField(TEXT("cameraPitch"), Event.CameraPitch.GetValue());
	}

	Object->SetNumberField(TEXT("gameThreadMs"), Event.GameThreadMs);
	Object->SetNumberField(TEXT("renderThreadMs"), Event.RenderThreadMs);
	return Object;
}

FString GetOptionalString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	FString Value;
	Object->TryGetStringField(FieldName, Value);
	return Value;
}

double GetOptionalNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	double Value = 0.0;
	Object->TryGetNumberField(FieldName, Value);
	return Value;
}

int64 GetOptionalInt64String(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	FString Value;
	if (!Object->TryGetStringField(FieldName, Value))
	{
		return 0;
	}
	return FCString::Strtoi64(*Value, nullptr, 10);
}

void ReadStringMap(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TMap<FString, FString>& OutMap)
{
	const TSharedPtr<FJsonObject>* MapObject = nullptr;
	if (!Object->TryGetObjectField(FieldName, MapObject) || !MapObject || !MapObject->IsValid())
	{
		return;
	}

	// UE 5.8 changed FJsonObject::Values keys to UE::TSharedString, so a
	// const-ref TPair<FString,...> binds to a temporary there, while on <=5.7
	// (FString keys) a by-value TPair is a needless copy; Android clang
	// -Werror (-Wrange-loop-construct) rejects BOTH spellings depending on
	// engine version (Epic Fab bounced the 5.8.0 build; our 5.6 CI bounced
	// the by-value form). const auto& is the only form clean on 5.3-5.8, and
	// FString(Pair.Key) is the engine's own key-conversion idiom (see
	// JsonSerializerReader.cpp) -- do NOT rewrite to an explicit TPair type.
	for (const auto& Pair : (*MapObject)->Values)
	{
		FString Value;
		if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
		{
			OutMap.Add(FString(Pair.Key), Value);
		}
	}
}

void ReadDoubleMap(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TMap<FString, double>& OutMap)
{
	const TSharedPtr<FJsonObject>* MapObject = nullptr;
	if (!Object->TryGetObjectField(FieldName, MapObject) || !MapObject || !MapObject->IsValid())
	{
		return;
	}

	for (const auto& Pair : (*MapObject)->Values)
	{
		double Value = 0.0;
		if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(Value))
		{
			OutMap.Add(FString(Pair.Key), Value);
		}
	}
}

bool TryReadEvent(const TSharedPtr<FJsonObject>& Object, FFramedashEvent& OutEvent)
{
	if (!Object.IsValid())
	{
		return false;
	}

	OutEvent.EventName = GetOptionalString(Object, TEXT("eventName"));
	if (OutEvent.EventName.IsEmpty())
	{
		return false;
	}

	OutEvent.TimestampUs = GetOptionalInt64String(Object, TEXT("timestampUs"));
	OutEvent.SessionId = GetOptionalString(Object, TEXT("sessionId"));
	OutEvent.PlayerId = GetOptionalString(Object, TEXT("playerId"));

	const TSharedPtr<FJsonObject>* Position = nullptr;
	if (Object->TryGetObjectField(TEXT("position"), Position) && Position && Position->IsValid())
	{
		OutEvent.Position.X = GetOptionalNumber(*Position, TEXT("x"));
		OutEvent.Position.Y = GetOptionalNumber(*Position, TEXT("y"));
		OutEvent.Position.Z = GetOptionalNumber(*Position, TEXT("z"));
	}

	OutEvent.MapId = GetOptionalString(Object, TEXT("mapId"));
	OutEvent.Fps = static_cast<float>(GetOptionalNumber(Object, TEXT("fps")));
	OutEvent.FrameTimeMs = static_cast<float>(GetOptionalNumber(Object, TEXT("frameTimeMs")));
	OutEvent.MemoryUsedBytes = GetOptionalInt64String(Object, TEXT("memoryUsedBytes"));
	OutEvent.GpuTimeMs = static_cast<float>(GetOptionalNumber(Object, TEXT("gpuTimeMs")));
	ReadStringMap(Object, TEXT("attributes"), OutEvent.Attributes);
	ReadDoubleMap(Object, TEXT("metrics"), OutEvent.Metrics);

	double Source = static_cast<double>(EFramedashTelemetrySource::Player);
	Object->TryGetNumberField(TEXT("source"), Source);
	const uint8 SourceInt = static_cast<uint8>(Source);
	OutEvent.Source = SourceInt <= static_cast<uint8>(EFramedashTelemetrySource::Automated)
		? static_cast<EFramedashTelemetrySource>(SourceInt)
		: EFramedashTelemetrySource::Player;

	OutEvent.BuildId = GetOptionalString(Object, TEXT("buildId"));
	OutEvent.Platform = GetOptionalString(Object, TEXT("platform"));
	OutEvent.EngineVersion = GetOptionalString(Object, TEXT("engineVersion"));

	double CameraYaw = 0.0;
	if (Object->TryGetNumberField(TEXT("cameraYaw"), CameraYaw))
	{
		OutEvent.CameraYaw = static_cast<float>(CameraYaw);
	}

	double CameraPitch = 0.0;
	if (Object->TryGetNumberField(TEXT("cameraPitch"), CameraPitch))
	{
		OutEvent.CameraPitch = static_cast<float>(CameraPitch);
	}

	OutEvent.GameThreadMs = static_cast<float>(GetOptionalNumber(Object, TEXT("gameThreadMs")));
	OutEvent.RenderThreadMs = static_cast<float>(GetOptionalNumber(Object, TEXT("renderThreadMs")));
	return true;
}
} // namespace

TArray<FFramedashEvent> FNullPersistence::Load()
{
	return TArray<FFramedashEvent>();
}

bool FNullPersistence::Save(const TArray<FFramedashEvent>& Events)
{
	(void)Events;
	return true;
}

bool FNullPersistence::Append(const TArray<FFramedashEvent>& Events)
{
	(void)Events;
	return true;
}

bool FNullPersistence::DropOldest(int32 Count)
{
	(void)Count;
	return true;
}

bool FNullPersistence::Clear()
{
	return true;
}

FFilePersistence::FFilePersistence()
	: FFilePersistence(DefaultQueueFilePath())
{
}

FFilePersistence::FFilePersistence(FString InQueueFilePath)
	: QueueFilePath(MoveTemp(InQueueFilePath))
{
}

TArray<FFramedashEvent> FFilePersistence::Load()
{
	FScopeLock Lock(&QueueFileCriticalSection);
	return LoadFromDisk();
}

bool FFilePersistence::Save(const TArray<FFramedashEvent>& Events)
{
	FScopeLock Lock(&QueueFileCriticalSection);
	return SaveToDisk(Events);
}

bool FFilePersistence::Append(const TArray<FFramedashEvent>& Events)
{
	if (Events.Num() == 0)
	{
		return true;
	}

	FScopeLock Lock(&QueueFileCriticalSection);

	TArray<FFramedashEvent> ExistingEvents = LoadFromDisk();
	ExistingEvents.Append(Events);

	const int32 Overflow = ExistingEvents.Num() - FramedashConstants::MaxPersistedEvents;
	if (Overflow > 0)
	{
		ExistingEvents.RemoveAt(0, Overflow, FRAMEDASH_ALLOW_SHRINKING_NO);
		UE_LOG(LogFramedash, Warning, TEXT("Offline queue full. Dropped %d oldest persisted event(s)."), Overflow);
	}

	return SaveToDisk(ExistingEvents);
}

bool FFilePersistence::DropOldest(int32 Count)
{
	if (Count <= 0)
	{
		return true;
	}

	FScopeLock Lock(&QueueFileCriticalSection);

	TArray<FFramedashEvent> ExistingEvents = LoadFromDisk();
	if (ExistingEvents.Num() == 0)
	{
		return true;
	}

	if (Count >= ExistingEvents.Num())
	{
		return ClearFromDisk();
	}

	ExistingEvents.RemoveAt(0, Count, FRAMEDASH_ALLOW_SHRINKING_NO);
	return SaveToDisk(ExistingEvents);
}

bool FFilePersistence::Clear()
{
	FScopeLock Lock(&QueueFileCriticalSection);
	return ClearFromDisk();
}

FString FFilePersistence::DefaultQueueFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Framedash"), TEXT("offline-queue.json"));
}

bool FFilePersistence::SaveToDisk(const TArray<FFramedashEvent>& Events) const
{
	if (Events.Num() == 0)
	{
		return ClearFromDisk();
	}

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("version"), PersistenceVersion);

	TArray<TSharedPtr<FJsonValue>> EventValues;
	EventValues.Reserve(Events.Num());
	for (const FFramedashEvent& Event : Events)
	{
		EventValues.Add(MakeShared<FJsonValueObject>(EventToJsonObject(Event)));
	}
	RootObject->SetArrayField(TEXT("events"), MoveTemp(EventValues));

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(RootObject, Writer))
	{
		UE_LOG(LogFramedash, Warning, TEXT("Failed to serialize offline queue."));
		return false;
	}

	const FString Directory = FPaths::GetPath(QueueFilePath);
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		UE_LOG(LogFramedash, Warning, TEXT("Failed to create offline queue directory: %s"), *Directory);
		return false;
	}

	const FString TempPath = QueueFilePath + FString::Printf(TEXT(".%u.tmp"), FPlatformProcess::GetCurrentProcessId());
	if (!FFileHelper::SaveStringToFile(Json, *TempPath))
	{
		UE_LOG(LogFramedash, Warning, TEXT("Failed to write offline queue: %s"), *TempPath);
		return false;
	}

	if (!IFileManager::Get().Move(*QueueFilePath, *TempPath, true, true))
	{
		IFileManager::Get().Delete(*TempPath, false, true);
		UE_LOG(LogFramedash, Warning, TEXT("Failed to replace offline queue: %s"), *QueueFilePath);
		return false;
	}

	return true;
}

bool FFilePersistence::ClearFromDisk() const
{
	if (!IFileManager::Get().FileExists(*QueueFilePath))
	{
		return true;
	}
	return IFileManager::Get().Delete(*QueueFilePath, false, true);
}

TArray<FFramedashEvent> FFilePersistence::LoadFromDisk() const
{
	// Suppress disk-IO metering for the SDK's OWN offline-queue read on this
	// thread: recovering/maintaining persisted telemetry must not be reported as
	// game io.read_* (see FFramedashIoTrackingPlatformFile). The read below
	// (FFileHelper::LoadFileToString) is synchronous on the calling thread, so this
	// scope precisely covers it. No-op cost when disk-IO tracking is off.
	Framedash::FIoMeteringSuppressionScope IoMeteringSuppress;

	TArray<FFramedashEvent> Events;

	if (!IFileManager::Get().FileExists(*QueueFilePath))
	{
		return Events;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *QueueFilePath))
	{
		UE_LOG(LogFramedash, Warning, TEXT("Failed to read offline queue: %s"), *QueueFilePath);
		return Events;
	}

	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogFramedash, Warning, TEXT("Ignoring unreadable offline queue: %s"), *QueueFilePath);
		IFileManager::Get().Delete(*QueueFilePath, false, true);
		return Events;
	}

	double Version = 0.0;
	if (!RootObject->TryGetNumberField(TEXT("version"), Version) ||
		static_cast<int32>(Version) != PersistenceVersion)
	{
		UE_LOG(LogFramedash, Warning, TEXT("Ignoring incompatible offline queue: %s"), *QueueFilePath);
		IFileManager::Get().Delete(*QueueFilePath, false, true);
		return Events;
	}

	const TArray<TSharedPtr<FJsonValue>>* EventValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("events"), EventValues) || !EventValues)
	{
		return Events;
	}

	Events.Reserve(EventValues->Num());
	for (const TSharedPtr<FJsonValue>& Value : *EventValues)
	{
		FFramedashEvent Event;
		if (Value.IsValid() && TryReadEvent(Value->AsObject(), Event))
		{
			Events.Add(MoveTemp(Event));
		}
	}

	return Events;
}
