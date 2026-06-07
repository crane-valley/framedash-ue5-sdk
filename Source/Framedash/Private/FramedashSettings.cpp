// Copyright Crane Valley. All Rights Reserved.

#include "FramedashSettings.h"

UFramedashSettings::UFramedashSettings()
	: EndpointUrl(TEXT("https://ingest.framedash.dev/v1/events"))
	, SamplingRate(1.0f)
	, bAutoInitialize(true)
	, bEnableOfflineQueue(true)
{
}
