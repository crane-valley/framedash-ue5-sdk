// Copyright 2026 Crane Valley. All Rights Reserved.

#include "FramedashEditorLogic.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace FramedashEditor
{
namespace
{
	bool DeserializeObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Field, OutValue);
	}

	bool ReadNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& OutValue)
	{
		return Object.IsValid() && Object->TryGetNumberField(Field, OutValue) && FMath::IsFinite(OutValue);
	}

	bool ReadNumberOrNumericString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		double& OutValue)
	{
		if (ReadNumber(Object, Field, OutValue))
		{
			return true;
		}

		FString StringValue;
		if (!Object.IsValid() || !Object->TryGetStringField(Field, StringValue) || !StringValue.IsNumeric())
		{
			return false;
		}
		OutValue = FCString::Atod(*StringValue);
		return FMath::IsFinite(OutValue);
	}

	bool ReadOptionalNumber(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		TOptional<double>& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonValue>* ValuePtr = Object->Values.Find(Field);
		if (ValuePtr == nullptr)
		{
			OutValue.Reset();
			return true;
		}
		if (!ValuePtr->IsValid())
		{
			return false;
		}
		const FJsonValue& Value = **ValuePtr;
		if (Value.Type == EJson::Null)
		{
			OutValue.Reset();
			return true;
		}

		double Number = 0.0;
		if (!Value.TryGetNumber(Number) || !FMath::IsFinite(Number))
		{
			return false;
		}
		OutValue = Number;
		return true;
	}

	bool ReadSuccessfulDataArray(
		const FString& Json,
		const TCHAR* ShapeName,
		TSharedPtr<FJsonObject>& OutRoot,
		const TArray<TSharedPtr<FJsonValue>>*& OutData,
		FString& OutError)
	{
		OutData = nullptr;
		if (!DeserializeObject(Json, OutRoot))
		{
			OutError = FString::Printf(TEXT("Malformed %s response."), ShapeName);
			return false;
		}

		bool bSuccess = false;
		if (!OutRoot->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess ||
			!OutRoot->TryGetArrayField(TEXT("data"), OutData) || OutData == nullptr)
		{
			OutError = FString::Printf(TEXT("Malformed %s response."), ShapeName);
			return false;
		}
		return true;
	}
}

bool ParseMapsResponse(const FString& Json, TArray<FMapInfo>& OutMaps, FString& OutError)
{
	OutMaps.Reset();
	OutError.Reset();

	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
	if (!ReadSuccessfulDataArray(Json, TEXT("maps"), Root, Data, OutError))
	{
		return false;
	}

	OutMaps.Reserve(Data->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Data)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr) || ObjectPtr == nullptr)
		{
			OutMaps.Reset();
			OutError = TEXT("Malformed map entry in maps response.");
			return false;
		}
		const TSharedPtr<FJsonObject> Object = *ObjectPtr;
		FMapInfo Map;
		if (!ReadString(Object, TEXT("id"), Map.Id) ||
			!ReadString(Object, TEXT("name"), Map.Name) ||
			!ReadString(Object, TEXT("mapId"), Map.MapId) ||
			!ReadString(Object, TEXT("imageUrl"), Map.ImageUrl) ||
			!ReadNumber(Object, TEXT("worldMinX"), Map.WorldMinX) ||
			!ReadNumber(Object, TEXT("worldMinY"), Map.WorldMinY) ||
			!ReadNumber(Object, TEXT("worldMaxX"), Map.WorldMaxX) ||
			!ReadNumber(Object, TEXT("worldMaxY"), Map.WorldMaxY) ||
			!ReadOptionalNumber(Object, TEXT("worldMinZ"), Map.WorldMinZ) ||
			!ReadOptionalNumber(Object, TEXT("worldMaxZ"), Map.WorldMaxZ) ||
			!ReadNumber(Object, TEXT("imageWidth"), Map.ImageWidth) ||
			!ReadNumber(Object, TEXT("imageHeight"), Map.ImageHeight) ||
			!ReadString(Object, TEXT("createdAt"), Map.CreatedAt) ||
			!ReadString(Object, TEXT("updatedAt"), Map.UpdatedAt))
		{
			OutMaps.Reset();
			OutError = TEXT("Malformed map entry in maps response.");
			return false;
		}
		OutMaps.Add(MoveTemp(Map));
	}
	return true;
}

bool ParseHeatmapResponse(const FString& Json, TArray<FHeatmapCell>& OutCells, FString& OutError)
{
	OutCells.Reset();
	OutError.Reset();

	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
	if (!ReadSuccessfulDataArray(Json, TEXT("heatmap"), Root, Data, OutError))
	{
		return false;
	}

	OutCells.Reserve(Data->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Data)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr) || ObjectPtr == nullptr)
		{
			OutCells.Reset();
			OutError = TEXT("Malformed cell entry in heatmap response.");
			return false;
		}
		const TSharedPtr<FJsonObject> Object = *ObjectPtr;
		FHeatmapCell Cell;
		if (!ReadNumber(Object, TEXT("x"), Cell.X) ||
			!ReadNumber(Object, TEXT("y"), Cell.Y) ||
			!ReadNumberOrNumericString(Object, TEXT("weight"), Cell.Weight) ||
			!ReadNumberOrNumericString(Object, TEXT("event_count"), Cell.EventCount) ||
			!ReadNumber(Object, TEXT("avg_fps"), Cell.AverageFps) ||
			!ReadNumber(Object, TEXT("avg_frame_time"), Cell.AverageFrameTime) ||
			!ReadNumber(Object, TEXT("avg_memory"), Cell.AverageMemory) ||
			!ReadOptionalNumber(Object, TEXT("avg_gpu_time"), Cell.AverageGpuTime) ||
			!ReadOptionalNumber(Object, TEXT("avg_mem_vram"), Cell.AverageVramMemory))
		{
			OutCells.Reset();
			OutError = TEXT("Malformed cell entry in heatmap response.");
			return false;
		}
		OutCells.Add(MoveTemp(Cell));
	}
	return true;
}

FString ParseProblemMessage(const FString& Json, const FString& Fallback)
{
	TSharedPtr<FJsonObject> Root;
	if (!DeserializeObject(Json, Root))
	{
		return Fallback;
	}

	FString Message;
	if (Root->TryGetStringField(TEXT("detail"), Message) && !Message.IsEmpty())
	{
		return Message;
	}
	if (Root->TryGetStringField(TEXT("title"), Message) && !Message.IsEmpty())
	{
		return Message;
	}
	return Fallback;
}

FCellRect BuildCellRect(const FHeatmapCell& Cell, const FMapInfo& Map, double CellSize)
{
	FCellRect Rect;
	if (CellSize <= 0.0 || !FMath::IsFinite(CellSize))
	{
		return Rect;
	}

	const double BinX = FMath::FloorToDouble((Cell.X - Map.WorldMinX) / CellSize);
	const double BinY = FMath::FloorToDouble((Cell.Y - Map.WorldMinY) / CellSize);
	Rect.MinX = Map.WorldMinX + BinX * CellSize;
	Rect.MinY = Map.WorldMinY + BinY * CellSize;
	Rect.MaxX = FMath::Min(Rect.MinX + CellSize, Map.WorldMaxX);
	Rect.MaxY = FMath::Min(Rect.MinY + CellSize, Map.WorldMaxY);
	return Rect;
}

double FindMaxWeight(const TArray<FHeatmapCell>& Cells)
{
	double MaxWeight = 0.0;
	for (const FHeatmapCell& Cell : Cells)
	{
		MaxWeight = FMath::Max(MaxWeight, Cell.Weight);
	}
	return MaxWeight;
}

double NormalizeWeight(double Weight, double MaxWeight)
{
	if (!FMath::IsFinite(Weight) || !FMath::IsFinite(MaxWeight) || MaxWeight <= 0.0)
	{
		return 0.0;
	}
	return FMath::Clamp(Weight / MaxWeight, 0.0, 1.0);
}

FLinearColor HeatmapColor(double NormalizedWeight, float Opacity)
{
	const float Alpha = FMath::Clamp(Opacity, 0.0f, 1.0f);
	FLinearColor Color = FMath::Lerp(
		FLinearColor(0.0f, 0.0f, 1.0f, Alpha),
		FLinearColor(1.0f, 0.0f, 0.0f, Alpha),
		static_cast<float>(FMath::Clamp(NormalizedWeight, 0.0, 1.0)));
	Color.A = Alpha;
	return Color;
}
}
