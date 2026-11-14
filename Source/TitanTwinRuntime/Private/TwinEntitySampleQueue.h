// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/ThreadSafeCounter64.h"
#include "Misc/ScopeLock.h"
#include "TitanTwinTypes.h"

inline FThreadSafeCounter64& TwinPendingEntityBytes()
{
	static FThreadSafeCounter64 Bytes;
	return Bytes;
}

struct FTwinEntitySampleQueue
{
	struct FEntry { FName Property; FTwinPropertySample Sample; int64 Bytes = 0; };
	FCriticalSection Mutex;
	TQueue<FEntry, EQueueMode::Mpsc> Queue;
	int32 Count = 0;
	int64 Bytes = 0;
	int64 Dropped = 0;
	uint64 Generation = 1;
	bool bAccepting = true;
	bool bScheduled = false;

	~FTwinEntitySampleQueue() { TwinPendingEntityBytes().Subtract(Bytes); }
	void Stop()
	{
		FScopeLock Lock(&Mutex);
		bAccepting = false;
		bScheduled = false;
		++Generation;
		FEntry Entry;
		while (Queue.Dequeue(Entry)) {}
		TwinPendingEntityBytes().Subtract(Bytes);
		Dropped += Count;
		Count = 0;
		Bytes = 0;
	}
};
