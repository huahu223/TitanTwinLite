// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinDataSourceIngress.h"

#include "Misc/ScopeLock.h"

FTwinDataSourceIngress::FTwinDataSourceIngress(int32 InMaxPendingPoints, int64 InMaxPendingBytes)
	: MaxPendingPoints(FMath::Max(1, InMaxPendingPoints))
	, MaxPendingBytes(FMath::Clamp<int64>(InMaxPendingBytes, 65536, 268435456))
{
}

bool FTwinDataSourceIngress::Enqueue(const FTwinDataPoint& Point)
{
	if (!Point.IsValid())
	{
		InvalidCount.Increment();
		return false;
	}
	const int64 Bytes = sizeof(FQueuedPoint) + Point.Address.GetAllocatedSize()
		+ Point.Value.TextValue.GetAllocatedSize();
	FScopeLock Lock(&AcceptanceMutex);
	if (!bAccepting)
	{
		return false;
	}

	if (PendingCount.GetValue() >= MaxPendingPoints || Bytes > MaxPendingBytes - PendingBytes.GetValue())
	{
		DroppedCount.Increment();
		return false;
	}

	FQueuedPoint Entry;
	Entry.Point = Point;
	Entry.ReservedBytes = Bytes;
	PendingCount.Increment();
	PendingBytes.Add(Bytes);
	Queue.Enqueue(MoveTemp(Entry));
	return true;
}

bool FTwinDataSourceIngress::Dequeue(FTwinDataPoint& OutPoint)
{
	FQueuedPoint Entry;
	if (!Queue.Dequeue(Entry))
	{
		return false;
	}

	PendingCount.Decrement();
	PendingBytes.Subtract(Entry.ReservedBytes);
	OutPoint = MoveTemp(Entry.Point);
	return true;
}

int32 FTwinDataSourceIngress::Drain(TArray<FTwinDataPoint>& OutPoints, int32 MaxPoints)
{
	const int32 Limit = FMath::Max(0, MaxPoints);
	int32 Drained = 0;
	FTwinDataPoint Point;
	while (Drained < Limit && Dequeue(Point))
	{
		OutPoints.Add(MoveTemp(Point));
		++Drained;
	}
	return Drained;
}

void FTwinDataSourceIngress::StopAccepting()
{
	FScopeLock Lock(&AcceptanceMutex);
	bAccepting = false;
}

int32 FTwinDataSourceIngress::GetPendingCount() const
{
	return PendingCount.GetValue();
}

int32 FTwinDataSourceIngress::GetDroppedCount() const
{
	return DroppedCount.GetValue();
}

int32 FTwinDataSourceIngress::GetInvalidCount() const { return InvalidCount.GetValue(); }
int64 FTwinDataSourceIngress::GetPendingBytes() const { return PendingBytes.GetValue(); }
