// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TitanTwinMqtt.h"
#include "TwinMqttNativeClient.h"

DEFINE_LOG_CATEGORY(LogTitanTwinMqtt);

void FTitanTwinMqttModule::ShutdownModule()
{
	FTwinMqttNativeClient::ShutdownResolvers();
}
IMPLEMENT_MODULE(FTitanTwinMqttModule, TitanTwinMqtt)
