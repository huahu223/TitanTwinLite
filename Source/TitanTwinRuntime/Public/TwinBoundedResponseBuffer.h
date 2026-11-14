// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

/** HTTP receive callbacks can use this from their worker thread without touching UObjects. */
class FTwinBoundedResponseBuffer
{
public:
	explicit FTwinBoundedResponseBuffer(int64 InLimit)
		: Limit(FMath::Clamp<int64>(InLimit, 1, 64 * 1024 * 1024)) {}

	bool Append(const void* Data, int64 Count)
	{
		FScopeLock Lock(&Mutex);
		if (Count < 0 || (Count > 0 && !Data) || Count > Limit - Bytes.Num())
		{
			bExceeded = true;
			return false;
		}
		if (Count > 0) Bytes.Append(static_cast<const uint8*>(Data), static_cast<int32>(Count));
		return true;
	}

	bool HasExceededLimit() const { FScopeLock Lock(&Mutex); return bExceeded; }
	FString ReadUtf8() const
	{
		FScopeLock Lock(&Mutex);
		if (Bytes.IsEmpty()) return FString();
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		FString Result(Converted.Length(), Converted.Get());
		if (!Result.IsEmpty() && Result[0] == 0xfeff) Result.RightChopInline(1);
		return Result;
	}

private:
	mutable FCriticalSection Mutex;
	TArray<uint8> Bytes;
	const int64 Limit;
	bool bExceeded = false;
};
