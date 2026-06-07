// Copyright Crane Valley. All Rights Reserved.

#include "Framedash.h"

#define LOCTEXT_NAMESPACE "FFramedashModule"

DEFINE_LOG_CATEGORY(LogFramedash);

void FFramedashModule::StartupModule()
{
	UE_LOG(LogFramedash, Log, TEXT("Framedash SDK v0.1.0 module loaded"));
}

void FFramedashModule::ShutdownModule()
{
	UE_LOG(LogFramedash, Log, TEXT("Framedash SDK module unloaded"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFramedashModule, Framedash)
