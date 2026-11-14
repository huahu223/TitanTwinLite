// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/ThreadSafeCounter64.h"
#include "HAL/CriticalSection.h"
#include "TwinDataSourceTypes.h"

// 这是 WorldSubsystem 管理的线程安全入口。协议适配器可以保存它的共享引用，
// 然后直接在工作线程发布数据，不需要碰 UObject。
class TITANTWINDATASOURCE_API FTwinDataSourceIngress final
{
public:
	explicit FTwinDataSourceIngress(int32 InMaxPendingPoints, int64 InMaxPendingBytes = 32 * 1024 * 1024);

	bool Enqueue(const FTwinDataPoint& Point);
	bool Dequeue(FTwinDataPoint& OutPoint);
	int32 Drain(TArray<FTwinDataPoint>& OutPoints, int32 MaxPoints);
	void StopAccepting();
	int32 GetPendingCount() const;
	int32 GetDroppedCount() const;
	int32 GetInvalidCount() const;
	int64 GetPendingBytes() const;

private:
	struct FQueuedPoint { FTwinDataPoint Point; int64 ReservedBytes = 0; };
	TQueue<FQueuedPoint, EQueueMode::Mpsc> Queue;
	FThreadSafeCounter PendingCount;
	FThreadSafeCounter DroppedCount;
	FThreadSafeCounter InvalidCount;
	FThreadSafeCounter64 PendingBytes;
	FThreadSafeBool bAccepting = true;
	mutable FCriticalSection AcceptanceMutex;
	const int32 MaxPendingPoints;
	const int64 MaxPendingBytes;
};
