// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"

inline int64 TwinPersistenceLineBytes(const FString& Line)
{ return sizeof(FString) + Line.GetAllocatedSize(); }

inline int64 TwinPersistenceQueueBytes(const TArray<FString>& Lines)
{
	int64 Bytes = 0;
	for (const FString& Line : Lines) Bytes += TwinPersistenceLineBytes(Line);
	return Bytes;
}

inline int32 TwinTrimPersistenceQueue(
	TArray<FString>& Lines, int32 MaxCount, int64 MaxBytes, int64& Bytes)
{
	Bytes = TwinPersistenceQueueBytes(Lines);
	MaxCount = FMath::Clamp(MaxCount, 1, 1048576);
	MaxBytes = FMath::Clamp<int64>(MaxBytes, 65536, 67108864);
	int32 Remove = 0;
	while (Remove < Lines.Num() && (Lines.Num() - Remove > MaxCount || Bytes > MaxBytes))
		Bytes -= TwinPersistenceLineBytes(Lines[Remove++]);
	if (Remove) Lines.RemoveAt(0, Remove, EAllowShrinking::No);
	return Remove;
}

inline bool TwinQueuePersistenceLine(
	TArray<FString>& Lines, FString&& Line, int32 MaxCount, int64 MaxBytes,
	int64& Bytes, int32& Dropped)
{
	MaxCount = FMath::Clamp(MaxCount, 1, 1048576);
	MaxBytes = FMath::Clamp<int64>(MaxBytes, 65536, 67108864);
	const int64 NewBytes = TwinPersistenceLineBytes(Line);
	if (NewBytes > MaxBytes) { ++Dropped; return false; }
	int32 Remove = 0;
	while (Remove < Lines.Num() && (Lines.Num() - Remove >= MaxCount || NewBytes > MaxBytes - Bytes))
		Bytes -= TwinPersistenceLineBytes(Lines[Remove++]);
	if (Remove) { Lines.RemoveAt(0, Remove, EAllowShrinking::No); Dropped += Remove; }
	Lines.Add(MoveTemp(Line));
	Bytes += NewBytes;
	return true;
}
