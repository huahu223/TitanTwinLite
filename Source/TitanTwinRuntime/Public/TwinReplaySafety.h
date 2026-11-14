// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * 这里协调当前进程里的回放和向外发送服务。
 * 调用这里的代码都应该在游戏线程运行。
 */
class TITANTWINRUNTIME_API FTwinReplaySafety final
{
public:
	static void SetLiveReplayActive(UWorld* World, bool bActive);
	static bool IsLiveReplayActive(const UWorld* World);
};
