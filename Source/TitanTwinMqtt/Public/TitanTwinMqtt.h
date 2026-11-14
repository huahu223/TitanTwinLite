// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogTitanTwinMqtt, Log, All);

class FTitanTwinMqttModule final : public IModuleInterface
{
public:
	virtual void ShutdownModule() override;
	virtual bool SupportsDynamicReloading() override { return false; }
};
