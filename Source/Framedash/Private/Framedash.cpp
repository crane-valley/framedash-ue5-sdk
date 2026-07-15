// Copyright 2026 Crane Valley. All Rights Reserved.

#include "Framedash.h"

#include "FramedashTypes.h"

#define LOCTEXT_NAMESPACE "FFramedashModule"

DEFINE_LOG_CATEGORY(LogFramedash);

void FFramedashModule::StartupModule()
{
	// Single source of truth for the version string (release gotcha: the log
	// used to carry its own copy and needed a manual bump every release).
	UE_LOG(LogFramedash, Log, TEXT("Framedash SDK v%s module loaded"), FRAMEDASH_SDK_VERSION);
}

void FFramedashModule::ShutdownModule()
{
	UE_LOG(LogFramedash, Log, TEXT("Framedash SDK module unloaded"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFramedashModule, Framedash)
