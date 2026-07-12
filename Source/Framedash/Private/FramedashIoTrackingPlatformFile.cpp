// Copyright Crane Valley. All Rights Reserved.

#include "FramedashIoTrackingPlatformFile.h"

#include "FramedashIoStats.h"
#include "Framedash.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"

// Full definitions of IAsyncReadFileHandle / IMappedFileHandle are needed to
// define the TValueOrError<TUniquePtr<...>>-returning overrides out-of-line
// (their destructors delete the handle, which must be a complete type -- see the
// header). Only pulled in where those overrides exist (UE 5.6+).
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
#include "Async/AsyncFileHandle.h"
#include "Async/MappedFileHandle.h"
#endif

namespace
{
	// The single installed wrapper. Set once in Install() and never cleared:
	// the wrapper is never unchained (see the class header), so this pointer
	// stays valid for the process lifetime. Used to re-enable/disable counting
	// without re-wrapping. Main-thread only (subsystem init/deinit).
	FFramedashIoTrackingPlatformFile* GInstalledWrapper = nullptr;

	// Reference count of subsystems that want metering enabled. Install()
	// increments, Disable() decrements; metering (the wrapper's bEnabled flag) is
	// on iff this is > 0, so one GameInstance shutting down does not stop metering
	// for others still running (multi-client PIE). Touched on the main thread only
	// but kept atomic for cheap safety alongside the atomic bEnabled flag.
	std::atomic<int32> GEnableRefCount{0};

	/**
	 * A transparent IFileHandle that meters IFileHandle::Read into the global IO
	 * stats accumulator, delegating every operation to the wrapped lower handle.
	 * Owns the lower handle (the wrapper hands ownership over on OpenRead).
	 * Counting is gated by a pointer to the owning platform file's atomic flag,
	 * which is valid for the process lifetime (the wrapper is never freed).
	 */
	class FFramedashIoTrackingFileHandle final : public IFileHandle
	{
	public:
		FFramedashIoTrackingFileHandle(IFileHandle* InLower, const std::atomic<bool>* InEnabled)
			: Lower(InLower)
			, EnabledFlag(InEnabled)
		{
		}

		virtual ~FFramedashIoTrackingFileHandle() override
		{
			delete Lower;
		}

		virtual int64 Tell() override { return Lower->Tell(); }
		virtual bool Seek(int64 NewPosition) override { return Lower->Seek(NewPosition); }
		virtual bool SeekFromEnd(int64 NewPositionRelativeToEnd = 0) override { return Lower->SeekFromEnd(NewPositionRelativeToEnd); }
		virtual int64 Size() override { return Lower->Size(); }
		virtual bool Write(const uint8* Source, int64 BytesToWrite) override { return Lower->Write(Source, BytesToWrite); }
		virtual bool Flush(const bool bFullFlush = false) override { return Lower->Flush(bFullFlush); }
		virtual bool Truncate(int64 NewSize) override { return Lower->Truncate(NewSize); }

		virtual bool Read(uint8* Destination, int64 BytesToRead) override
		{
			// Count only when enabled and the read succeeds. Time the delegated
			// call with the monotonic platform clock; on a non-monotonic clock a
			// backwards delta is clamped to 0 so a garbage timing can never emit a
			// negative/non-finite metric (the accumulator also drops those).
			if (!EnabledFlag->load(std::memory_order_relaxed))
			{
				return Lower->Read(Destination, BytesToRead);
			}
			const double StartSeconds = FPlatformTime::Seconds();
			const bool bOk = Lower->Read(Destination, BytesToRead);
			if (bOk)
			{
				// Clamp a backwards delta (non-monotonic clock) to 0 so the byte count
				// still accumulates as a valid 0ms sample instead of the accumulator
				// dropping the time component (keeps the comment above truthful).
				const double ElapsedMs = FMath::Max(0.0, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
				Framedash::GlobalIoStats().AddRead(static_cast<int64_t>(BytesToRead), ElapsedMs);
				Framedash::GlobalIoStats().AddOps(1);
			}
			return bOk;
		}

#if !UE_VERSION_OLDER_THAN(5, 5, 0)
		// ReadAt (positional pread-style read) was added to IFileHandle in UE 5.5.
		// Delegate only -- like the async/mapped paths it stays unmetered; the
		// metered surface is the sequential OpenRead Read path (documented gap).
		virtual bool ReadAt(uint8* Destination, int64 BytesToRead, int64 Offset) override
		{
			return Lower->ReadAt(Destination, BytesToRead, Offset);
		}
#endif

	private:
		IFileHandle* Lower;
		const std::atomic<bool>* EnabledFlag;
	};
} // namespace

void FFramedashIoTrackingPlatformFile::Install()
{
	// One more subsystem wants metering. Balanced by exactly one Disable() in that
	// subsystem's Deinitialize (it tracks whether it installed).
	GEnableRefCount.fetch_add(1, std::memory_order_relaxed);

	// Already installed (e.g. a second GameInstance, or a Shutdown()+re-init
	// cycle): just ensure counting is enabled on the existing chained wrapper.
	// Never wrap twice.
	if (GInstalledWrapper != nullptr)
	{
		GInstalledWrapper->bEnabled.store(true, std::memory_order_relaxed);
		return;
	}

	FPlatformFileManager& Manager = FPlatformFileManager::Get();
	IPlatformFile* Current = &Manager.GetPlatformFile();
	if (Current == nullptr)
	{
		UE_LOG(LogFramedash, Warning, TEXT("Disk IO tracking: no active platform file; not installed."));
		return;
	}

	// new (not MakePimpl): the wrapper is intentionally leaked for the process
	// lifetime because it is installed into the platform-file chain and never
	// unchained (unchaining mid-run is unsafe -- other systems hold the pointer).
	FFramedashIoTrackingPlatformFile* Wrapper = new FFramedashIoTrackingPlatformFile();
	if (!Wrapper->Initialize(Current, FCommandLine::Get()))
	{
		UE_LOG(LogFramedash, Warning, TEXT("Disk IO tracking: wrapper Initialize failed; not installed."));
		delete Wrapper;
		return;
	}

	Manager.SetPlatformFile(*Wrapper);
	GInstalledWrapper = Wrapper;
	UE_LOG(LogFramedash, Log, TEXT("Disk IO tracking installed (io.* metrics on perf_heartbeat)."));
}

void FFramedashIoTrackingPlatformFile::Disable()
{
	// One subsystem no longer wants metering. Only actually stop counting once the
	// reference count reaches zero -- another still-running GameInstance keeps it
	// on. Callers invoke this exactly once per Install() (guarded by their own
	// installed flag), so the count cannot go negative; clamp defensively anyway.
	const int32 Remaining = GEnableRefCount.fetch_sub(1, std::memory_order_relaxed) - 1;
	if (Remaining <= 0)
	{
		if (Remaining < 0)
		{
			GEnableRefCount.store(0, std::memory_order_relaxed);
		}
		if (GInstalledWrapper != nullptr)
		{
			GInstalledWrapper->bEnabled.store(false, std::memory_order_relaxed);
		}
	}
}

bool FFramedashIoTrackingPlatformFile::ShouldBeUsed(IPlatformFile* /*Inner*/, const TCHAR* /*CmdLine*/) const
{
	// Installed explicitly by the subsystem (not via the -FileWrapper command
	// line), so this predicate is not consulted for auto-insertion.
	return false;
}

bool FFramedashIoTrackingPlatformFile::Initialize(IPlatformFile* Inner, const TCHAR* /*CmdLine*/)
{
	if (Inner == nullptr)
	{
		return false;
	}
	LowerLevel = Inner;
	return true;
}

IFileHandle* FFramedashIoTrackingPlatformFile::OpenRead(const TCHAR* Filename, bool bAllowWrite)
{
	IFileHandle* Handle = LowerLevel->OpenRead(Filename, bAllowWrite);
	if (Handle == nullptr)
	{
		return nullptr;
	}
	// Return the lower handle UNWRAPPED (no metering, no wrapper allocation) when
	// either counting is disabled, or the current thread is inside an SDK-internal
	// read scope. The latter excludes the SDK's own offline-queue persistence reads
	// (FFramedashPersistenceProvider brackets its synchronous FFileHelper read with
	// FIoMeteringSuppressionScope) so telemetry recovery is not reported as game
	// disk-IO. Re-enabling later meters only handles opened after.
	if (!bEnabled.load(std::memory_order_relaxed) || Framedash::IsThreadReadSuppressed())
	{
		return Handle;
	}
	return new FFramedashIoTrackingFileHandle(Handle, &bEnabled);
}

// Out-of-line delegating overrides for the overloads that return a move-only
// TValueOrError<TUniquePtr<...>> (see the header note on C4150). Delegation
// only -- these async / memory-mapped paths are not metered.
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
FFileOpenAsyncResult FFramedashIoTrackingPlatformFile::OpenAsyncRead(const TCHAR* Filename, EOpenReadFlags Flags)
{
	return LowerLevel->OpenAsyncRead(Filename, Flags);
}

FOpenMappedResult FFramedashIoTrackingPlatformFile::OpenMappedEx(const TCHAR* Filename, EOpenReadFlags OpenOptions, int64 MaximumSize)
{
	return LowerLevel->OpenMappedEx(Filename, OpenOptions, MaximumSize);
}
#endif

#if !UE_VERSION_OLDER_THAN(5, 8, 0)
FOpenMappedResult FFramedashIoTrackingPlatformFile::OpenMappedEx2(const TCHAR* Filename, EOpenMappedFlags OpenOptions, int64 MaximumSize)
{
	return LowerLevel->OpenMappedEx2(Filename, OpenOptions, MaximumSize);
}
#endif
