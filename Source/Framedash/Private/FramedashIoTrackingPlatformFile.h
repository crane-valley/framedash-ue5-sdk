// Copyright 2026 Crane Valley. All Rights Reserved.
//
// A transparent IPlatformFile chaining wrapper that counts synchronous disk
// reads into the process-wide Framedash::GlobalIoStats() accumulator, feeding
// the io.* metrics attached to perf_heartbeat. It wraps every IFileHandle
// returned by OpenRead so IFileHandle::Read byte counts and wall time are
// summed; every other file operation delegates faithfully to the wrapped
// (lower) platform file so behavior is unchanged.
//
// Faithful delegation (IMPORTANT): a chaining IPlatformFile that overrides only
// the PURE virtuals would let this class's inherited NON-pure defaults silently
// replace the lower level's specialized implementations. The worst offenders
// are the async / memory-mapped / no-buffering read paths: the base defaults
// build a generic sync-read-based async handle (OpenAsyncRead), return nullptr
// (OpenMapped), or drop pak specialization (OpenReadNoBuffering) -- so a
// packaged build sitting on FPakPlatformFile would severely regress ALL async
// asset loading if we did not delegate. Tick / SetAsyncMinimumPriority /
// lifecycle hooks are likewise starved if not forwarded. Therefore EVERY
// non-pure virtual whose base default does not already route through a pure
// virtual we delegate is overridden here to forward to LowerLevel. The
// async / mapped / no-buffering paths are delegation-ONLY (not metered) -- see
// the IoDispatcher/async known gap below.
//
// Engine-version drift: the async / mapped / no-buffering signatures changed
// across UE 5.3-5.8 (OpenAsyncRead gained bool/EOpenReadFlags overloads in 5.6;
// OpenReadNoBuffering gained an EOpenReadFlags overload in 5.7; OpenMapped was
// replaced by OpenMappedEx/OpenMappedEx2 by 5.8). Those overrides are guarded
// with UE_VERSION_OLDER_THAN (Misc/EngineVersionComparison.h, the same token
// FramedashEngineCompat.h uses) so the plugin compiles across its 5.3+ floor.
//
// Lifecycle (fail-safe, see UFramedashSubsystem):
//   - Install() is called once at subsystem init WHEN bTrackDiskIo is true. It
//     wraps FPlatformFileManager's current platform file and installs itself as
//     the active layer. Any failure is swallowed (SDKs must never crash the
//     game): on failure the lower layer stays active, untouched.
//   - The wrapper is installed ONCE for the process lifetime and is NEVER
//     unchained -- restoring the lower layer mid-run while other systems may
//     hold this pointer is unsafe. On subsystem Deinitialize, Disable() flips an
//     atomic flag so counting stops; the wrapper stays in the chain, delegating.
//   - A later re-init with bTrackDiskIo true re-enables counting via Install().
//
// Self-exclusion: the SDK's own offline-queue persistence
// (Project/Saved/Framedash/offline-queue.json) is read through this same wrapper,
// so restoring/maintaining that queue would otherwise be reported as game
// io.read_* -- a false spike exactly in telemetry-recovery sessions. The
// persistence provider brackets its synchronous read with
// Framedash::FIoMeteringSuppressionScope (a thread-local guard) and OpenRead
// returns the lower handle unwrapped while that scope is active on the calling
// thread. A thread-local scope is used rather than a path comparison, which is
// unreliable at this layer (the incoming Filename may be relative or absolute,
// with platform-specific case/separator normalization).
//
// KNOWN GAP: the IoDispatcher/IoStore path (zen loader, Nanite streaming, bulk
// data) and async / memory-mapped reads bypass or are not metered through the
// synchronous OpenRead handle, so raw counters undercount Nanite-heavy IO. Only
// synchronous OpenRead handles are metered; that gap is documented and deferred
// to Phase 2.

#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/EngineVersionComparison.h"

#include <atomic>

/**
 * Chaining IPlatformFile that meters synchronous reads. Transparent: all
 * operations delegate to the wrapped lower-level platform file; only the
 * OpenRead-returned handle is metered.
 */
class FFramedashIoTrackingPlatformFile final : public IPlatformFile
{
public:
	FFramedashIoTrackingPlatformFile() = default;

	/**
	 * Install the wrapper as the active platform file (or re-enable it if already
	 * installed from a prior init). Idempotent and fail-safe: any failure is
	 * logged and swallowed, leaving the existing platform file untouched. Marks
	 * the IO stats source active so the heartbeat begins attaching io.* keys.
	 * Must be called on the main thread during subsystem init.
	 */
	static void Install();

	/**
	 * Stop counting: flip the installed wrapper's enabled flag to false. The
	 * wrapper stays in the chain (never unchained) and keeps delegating. No-op if
	 * nothing was installed.
	 */
	static void Disable();

	// --- IPlatformFile: initialization / chain management -------------------
	virtual bool ShouldBeUsed(IPlatformFile* Inner, const TCHAR* CmdLine) const override;
	virtual bool Initialize(IPlatformFile* Inner, const TCHAR* CmdLine) override;
	virtual void InitializeAfterSetActive() override { LowerLevel->InitializeAfterSetActive(); }
	virtual void InitializeAfterProjectFilePath() override { LowerLevel->InitializeAfterProjectFilePath(); }
	virtual void MakeUniquePakFilesForTheseFiles(const TArray<TArray<FString>>& InFiles) override { LowerLevel->MakeUniquePakFilesForTheseFiles(InFiles); }
	virtual void InitializeNewAsyncIO() override { LowerLevel->InitializeNewAsyncIO(); }
	virtual void Tick() override { LowerLevel->Tick(); }
	virtual IPlatformFile* GetLowerLevel() override { return LowerLevel; }
	virtual void SetLowerLevel(IPlatformFile* NewLowerLevel) override { LowerLevel = NewLowerLevel; }
	virtual const TCHAR* GetName() const override { return TEXT("FramedashIoTracking"); }

	// --- IPlatformFile: file queries / mutations (pure passthrough) ---------
	virtual bool FileExists(const TCHAR* Filename) override { return LowerLevel->FileExists(Filename); }
	virtual int64 FileSize(const TCHAR* Filename) override { return LowerLevel->FileSize(Filename); }
	virtual bool DeleteFile(const TCHAR* Filename) override { return LowerLevel->DeleteFile(Filename); }
	virtual bool IsReadOnly(const TCHAR* Filename) override { return LowerLevel->IsReadOnly(Filename); }
	virtual bool MoveFile(const TCHAR* To, const TCHAR* From) override { return LowerLevel->MoveFile(To, From); }
	virtual bool SetReadOnly(const TCHAR* Filename, bool bNewReadOnlyValue) override { return LowerLevel->SetReadOnly(Filename, bNewReadOnlyValue); }
	virtual FDateTime GetTimeStamp(const TCHAR* Filename) override { return LowerLevel->GetTimeStamp(Filename); }
	virtual void SetTimeStamp(const TCHAR* Filename, FDateTime DateTime) override { LowerLevel->SetTimeStamp(Filename, DateTime); }
	virtual FDateTime GetAccessTimeStamp(const TCHAR* Filename) override { return LowerLevel->GetAccessTimeStamp(Filename); }
	virtual FDateTime GetTimeStampLocal(const TCHAR* Filename) override { return LowerLevel->GetTimeStampLocal(Filename); }
	virtual void GetTimeStampPair(const TCHAR* PathA, const TCHAR* PathB, FDateTime& OutTimeStampA, FDateTime& OutTimeStampB) override { LowerLevel->GetTimeStampPair(PathA, PathB, OutTimeStampA, OutTimeStampB); }
	virtual FString GetFilenameOnDisk(const TCHAR* Filename) override { return LowerLevel->GetFilenameOnDisk(Filename); }
	virtual ESymlinkResult IsSymlink(const TCHAR* Filename) override { return LowerLevel->IsSymlink(Filename); }
	virtual bool HasMarkOfTheWeb(FStringView Filename, FString* OutSourceURL = nullptr) override { return LowerLevel->HasMarkOfTheWeb(Filename, OutSourceURL); }
	virtual bool SetMarkOfTheWeb(FStringView Filename, bool bNewStatus, const FString* InSourceURL = nullptr) override { return LowerLevel->SetMarkOfTheWeb(Filename, bNewStatus, InSourceURL); }

	// --- IPlatformFile: handle factories ------------------------------------
	// OpenRead is the metered path: the returned handle is wrapped to count reads.
	virtual IFileHandle* OpenRead(const TCHAR* Filename, bool bAllowWrite = false) override;
	virtual IFileHandle* OpenWrite(const TCHAR* Filename, bool bAppend = false, bool bAllowRead = false) override { return LowerLevel->OpenWrite(Filename, bAppend, bAllowRead); }

	// No-buffering read: delegate only (preserves pak specialization; unmetered).
	virtual IFileHandle* OpenReadNoBuffering(const TCHAR* Filename, bool bAllowWrite = false) override { return LowerLevel->OpenReadNoBuffering(Filename, bAllowWrite); }
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
	virtual FFileOpenResult OpenReadNoBuffering(const TCHAR* Filename, EOpenReadFlags Flags) override { return LowerLevel->OpenReadNoBuffering(Filename, Flags); }
#endif

	// Async read: delegate only (the base default builds a generic sync-based
	// handle that would bypass FPakPlatformFile's precaching). Unmetered.
	// The overloads that return a TValueOrError<TUniquePtr<...>> (FFileOpenAsyncResult /
	// FOpenMappedResult) are defined OUT-OF-LINE in the .cpp: their return type destructs
	// a TUniquePtr to IAsyncReadFileHandle / IMappedFileHandle, which are only
	// forward-declared here, so an inline body would trip C4150 (delete of an incomplete
	// type, warnings-as-errors). The .cpp includes the full definitions.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
	virtual IAsyncReadFileHandle* OpenAsyncRead(const TCHAR* Filename) override { return LowerLevel->OpenAsyncRead(Filename); }
#else
	virtual IAsyncReadFileHandle* OpenAsyncRead(const TCHAR* Filename, bool bAllowWrite = false) override { return LowerLevel->OpenAsyncRead(Filename, bAllowWrite); }
	virtual FFileOpenAsyncResult OpenAsyncRead(const TCHAR* Filename, EOpenReadFlags Flags) override;
#endif
	virtual void SetAsyncMinimumPriority(EAsyncIOPriorityAndFlags MinPriority) override { LowerLevel->SetAsyncMinimumPriority(MinPriority); }

	// Memory-mapped read: delegate only (the base default returns nullptr).
	// Unmetered.
	// Deprecated-base override compiled only pre-5.6: on 5.6+ the engine has no
	// live IPlatformFile::OpenMapped(const TCHAR*) call sites (verified against
	// engine source) and IPlatformFile's own default OpenMapped body just
	// forwards to OpenMappedEx, so OpenMappedEx (overridden below) already
	// covers interception on 5.6+ without pulling in the UE_DEPRECATED(5.6, ...)
	// base method and its C4996 warning.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
	virtual IMappedFileHandle* OpenMapped(const TCHAR* Filename) override { return LowerLevel->OpenMapped(Filename); }
#endif
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
	virtual FOpenMappedResult OpenMappedEx(const TCHAR* Filename, EOpenReadFlags OpenOptions = EOpenReadFlags::None, int64 MaximumSize = 0) override;
#endif
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
	virtual FOpenMappedResult OpenMappedEx2(const TCHAR* Filename, EOpenMappedFlags OpenOptions = EOpenMappedFlags::None, int64 MaximumSize = 0) override;
#endif

	// --- IPlatformFile: directory operations --------------------------------
	virtual bool DirectoryExists(const TCHAR* Directory) override { return LowerLevel->DirectoryExists(Directory); }
	virtual bool CreateDirectory(const TCHAR* Directory) override { return LowerLevel->CreateDirectory(Directory); }
	virtual bool DeleteDirectory(const TCHAR* Directory) override { return LowerLevel->DeleteDirectory(Directory); }
	virtual FFileStatData GetStatData(const TCHAR* FilenameOrDirectory) override { return LowerLevel->GetStatData(FilenameOrDirectory); }
	virtual bool IterateDirectory(const TCHAR* Directory, IPlatformFile::FDirectoryVisitor& Visitor) override { return LowerLevel->IterateDirectory(Directory, Visitor); }
	virtual bool IterateDirectory(const TCHAR* Directory, IPlatformFile::FDirectoryVisitorFunc Visitor) override { return LowerLevel->IterateDirectory(Directory, Visitor); }
	virtual bool IterateDirectoryStat(const TCHAR* Directory, IPlatformFile::FDirectoryStatVisitor& Visitor) override { return LowerLevel->IterateDirectoryStat(Directory, Visitor); }
	virtual bool IterateDirectoryStat(const TCHAR* Directory, IPlatformFile::FDirectoryStatVisitorFunc Visitor) override { return LowerLevel->IterateDirectoryStat(Directory, Visitor); }
	virtual bool IterateDirectoryRecursively(const TCHAR* Directory, IPlatformFile::FDirectoryVisitor& Visitor) override { return LowerLevel->IterateDirectoryRecursively(Directory, Visitor); }
	virtual bool IterateDirectoryRecursively(const TCHAR* Directory, IPlatformFile::FDirectoryVisitorFunc Visitor) override { return LowerLevel->IterateDirectoryRecursively(Directory, Visitor); }
	virtual bool IterateDirectoryStatRecursively(const TCHAR* Directory, IPlatformFile::FDirectoryStatVisitor& Visitor) override { return LowerLevel->IterateDirectoryStatRecursively(Directory, Visitor); }
	virtual bool IterateDirectoryStatRecursively(const TCHAR* Directory, IPlatformFile::FDirectoryStatVisitorFunc Visitor) override { return LowerLevel->IterateDirectoryStatRecursively(Directory, Visitor); }
	virtual void FindFiles(TArray<FString>& FoundFiles, const TCHAR* Directory, const TCHAR* FileExtension) override { LowerLevel->FindFiles(FoundFiles, Directory, FileExtension); }
	virtual void FindFilesRecursively(TArray<FString>& FoundFiles, const TCHAR* Directory, const TCHAR* FileExtension) override { LowerLevel->FindFilesRecursively(FoundFiles, Directory, FileExtension); }
	virtual bool DeleteDirectoryRecursively(const TCHAR* Directory) override { return LowerLevel->DeleteDirectoryRecursively(Directory); }
	virtual bool CreateDirectoryTree(const TCHAR* Directory) override { return LowerLevel->CreateDirectoryTree(Directory); }
	virtual bool CopyFile(const TCHAR* To, const TCHAR* From, EPlatformFileRead ReadFlags = EPlatformFileRead::None, EPlatformFileWrite WriteFlags = EPlatformFileWrite::None) override { return LowerLevel->CopyFile(To, From, ReadFlags, WriteFlags); }
	virtual bool CopyDirectoryTree(const TCHAR* DestinationDirectory, const TCHAR* Source, bool bOverwriteAllExisting) override { return LowerLevel->CopyDirectoryTree(DestinationDirectory, Source, bOverwriteAllExisting); }
	virtual FString ConvertToAbsolutePathForExternalAppForRead(const TCHAR* Filename) override { return LowerLevel->ConvertToAbsolutePathForExternalAppForRead(Filename); }
	virtual FString ConvertToAbsolutePathForExternalAppForWrite(const TCHAR* Filename) override { return LowerLevel->ConvertToAbsolutePathForExternalAppForWrite(Filename); }

	// --- IPlatformFile: misc ------------------------------------------------
	virtual bool SendMessageToServer(const TCHAR* Message, IFileServerMessageHandler* Handler) override { return LowerLevel->SendMessageToServer(Message, Handler); }
	virtual bool DoesCreatePublicFiles() override { return LowerLevel->DoesCreatePublicFiles(); }
	virtual void SetCreatePublicFiles(bool bCreatePublicFiles) override { LowerLevel->SetCreatePublicFiles(bCreatePublicFiles); }

private:
	// The wrapped (lower) platform file. Never null once Initialize succeeds.
	IPlatformFile* LowerLevel = nullptr;

	// When false, OpenRead returns the lower handle unwrapped so no counting
	// occurs. Read by handles via a stable pointer (this wrapper is never freed).
	std::atomic<bool> bEnabled{true};
};
