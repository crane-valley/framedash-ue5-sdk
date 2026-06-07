// Copyright Crane Valley. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramedashTypes.h"

/**
 * Stores unsent telemetry so shutdowns and transient network failures do not
 * lose the whole in-memory queue. Implementations must stay fail-safe.
 */
class IPersistenceProvider
{
public:
	virtual ~IPersistenceProvider() = default;

	virtual TArray<FFramedashEvent> Load() = 0;
	virtual bool Save(const TArray<FFramedashEvent>& Events) = 0;
	virtual bool Append(const TArray<FFramedashEvent>& Events) = 0;
	virtual bool DropOldest(int32 Count) = 0;
	virtual bool Clear() = 0;
};

/** No-op persistence provider for projects that explicitly disable disk queueing. */
class FNullPersistence final : public IPersistenceProvider
{
public:
	virtual TArray<FFramedashEvent> Load() override;
	virtual bool Save(const TArray<FFramedashEvent>& Events) override;
	virtual bool Append(const TArray<FFramedashEvent>& Events) override;
	virtual bool DropOldest(int32 Count) override;
	virtual bool Clear() override;
};

/** File-backed persistence provider stored under Project/Saved/Framedash. */
class FFilePersistence final : public IPersistenceProvider
{
public:
	FFilePersistence();
	explicit FFilePersistence(FString InQueueFilePath);

	virtual TArray<FFramedashEvent> Load() override;
	virtual bool Save(const TArray<FFramedashEvent>& Events) override;
	virtual bool Append(const TArray<FFramedashEvent>& Events) override;
	virtual bool DropOldest(int32 Count) override;
	virtual bool Clear() override;

	static FString DefaultQueueFilePath();

private:
	TArray<FFramedashEvent> LoadFromDisk() const;
	bool SaveToDisk(const TArray<FFramedashEvent>& Events) const;
	bool ClearFromDisk() const;

	FString QueueFilePath;
};
