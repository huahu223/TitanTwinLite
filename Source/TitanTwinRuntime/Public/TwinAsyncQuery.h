// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"
#include <atomic>

class FTwinQueryTracker;

struct FTwinQueryControl
{
	TWeakPtr<FTwinQueryTracker, ESPMode::ThreadSafe> Tracker;
	std::atomic<bool> bCancelled{false};
	std::atomic<bool> bReservationReleased{false};
	double DeadlineSeconds = 0.0;
	bool ShouldStop() const { return bCancelled.load() || FPlatformTime::Seconds() >= DeadlineSeconds; }
	~FTwinQueryControl();
};

/** Cancellation does not release a slot until the queued/running task actually exits. */
class FTwinQueryTracker : public TSharedFromThis<FTwinQueryTracker, ESPMode::ThreadSafe>
{
public:
	bool Begin(FGuid Id, int32 Maximum, double Timeout,
		TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe>& OutControl)
	{
		TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe> Created;
		{
			FScopeLock Lock(&Mutex);
			if (!bActive || Outstanding >= Maximum || Requests.Contains(Id)) return false;
			Created = MakeShared<FTwinQueryControl, ESPMode::ThreadSafe>();
			Created->Tracker = AsShared();
			Created->DeadlineSeconds = FPlatformTime::Seconds() + Timeout;
			Requests.Add(Id, Created);
			++Outstanding;
		}
		OutControl = MoveTemp(Created);
		return true;
	}
	bool Complete(FGuid Id, const TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe>& Control)
	{
		TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe> KeepAlive;
		FScopeLock Lock(&Mutex);
		const auto* Found = Requests.Find(Id);
		if (!bActive || !Found || Found->Get() != Control.Get() || Control->bCancelled.load()) return false;
		KeepAlive = *Found;
		Requests.Remove(Id);
		if (!Control->bReservationReleased.exchange(true)) --Outstanding;
		return true;
	}
	bool Cancel(FGuid Id)
	{
		TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe> Control;
		{
			FScopeLock Lock(&Mutex);
			if (!Requests.RemoveAndCopyValue(Id, Control)) return false;
			Control->bCancelled.store(true);
		}
		return true;
	}
	void Stop()
	{
		TArray<TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe>> KeepAlive;
		{
			FScopeLock Lock(&Mutex);
			bActive = false;
			Requests.GenerateValueArray(KeepAlive);
			for (const auto& Control : KeepAlive) Control->bCancelled.store(true);
			Requests.Reset();
		}
	}
	void ReleaseReservation() { FScopeLock Lock(&Mutex); --Outstanding; }
private:
	FCriticalSection Mutex;
	TMap<FGuid, TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe>> Requests;
	int32 Outstanding = 0;
	bool bActive = true;
};

inline FTwinQueryControl::~FTwinQueryControl()
{
	if (!bReservationReleased.exchange(true))
		if (auto Owner = Tracker.Pin()) Owner->ReleaseReservation();
}

/** Read UTF-8 JSONL without duplicating an entire file into a string and line array. */
inline bool TwinReadJsonLines(
	const FString& Filename, int64 MaximumBytes, int64& BytesRead, bool& bTruncated,
	const TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe>& Control,
	TFunctionRef<bool(const FString&)> Visitor)
{
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
	if (!Reader) return false;
	TArray<uint8> Line;
	uint8 Chunk[32768];
	bool bDiscardLine = false;
	auto VisitLine = [&]()
	{
		if (Line.IsEmpty() || bDiscardLine) { Line.Reset(); bDiscardLine = false; return true; }
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Line.GetData()), Line.Num());
		FString Text(Converted.Length(), Converted.Get());
		Line.Reset();
		if (!Text.IsEmpty() && Text[0] == 0xfeff) Text.RightChopInline(1);
		return Visitor(Text);
	};
	while (Reader->Tell() < Reader->TotalSize() && !Control->ShouldStop())
	{
		const int64 Count = FMath::Min<int64>(sizeof(Chunk),
			FMath::Min(Reader->TotalSize() - Reader->Tell(), MaximumBytes - BytesRead));
		if (Count <= 0) { bTruncated = true; return true; }
		Reader->Serialize(Chunk, Count);
		if (Reader->IsError()) return false;
		BytesRead += Count;
		for (int64 Index = 0; Index < Count; ++Index)
		{
			if (Chunk[Index] == '\n') { if (!VisitLine()) return true; }
			else if (Chunk[Index] != '\r' && !bDiscardLine)
			{
				if (Line.Num() >= 1024 * 1024) { bDiscardLine = true; Line.Reset(); bTruncated = true; }
				else Line.Add(Chunk[Index]);
			}
		}
	}
	if (!Control->ShouldStop()) VisitLine();
	return true;
}

inline TArray<FString> TwinFindQueryFiles(
	const FString& Directory, const TSharedPtr<FTwinQueryControl, ESPMode::ThreadSafe>& Control, bool& bTruncated)
{
	struct FFile { FString Name; FDateTime Time; };
	TArray<FFile> Found;
	IFileManager::Get().IterateDirectoryRecursively(*Directory, [&](const TCHAR* Path, bool bDirectory)
	{
		if (Control->ShouldStop()) return false;
		if (!bDirectory && FStringView(Path).EndsWith(TEXT(".jsonl")))
		{
			if (Found.Num() >= 4096) { bTruncated = true; return false; }
			Found.Add({Path, IFileManager::Get().GetTimeStamp(Path)});
		}
		return true;
	});
	Found.Sort([](const FFile& A, const FFile& B) { return A.Time > B.Time; });
	TArray<FString> Files;
	for (FFile& File : Found) Files.Add(MoveTemp(File.Name));
	return Files;
}

template <typename RecordType, typename Predicate, typename SizeFunction, typename IdFunction>
inline void TwinKeepBoundedRecord(
	TArray<RecordType>& Records, TSet<FGuid>& SeenIds, int64& Bytes,
	int32 MaximumRecords, int64 MaximumBytes, RecordType&& Record,
	const Predicate& Weaker, const SizeFunction& Estimate, const IdFunction& GetId, bool& bTruncated)
{
	const FGuid Id = GetId(Record);
	if (SeenIds.Contains(Id)) return;
	const int64 Size = Estimate(Record);
	if (Size > MaximumBytes
		|| (Records.Num() >= MaximumRecords && Weaker(Record, Records[0])))
	{ bTruncated = true; return; }
	Bytes += Size;
	SeenIds.Add(Id);
	Records.HeapPush(MoveTemp(Record), Weaker);
	while (Records.Num() > MaximumRecords || Bytes > MaximumBytes)
	{
		RecordType Removed;
		Records.HeapPop(Removed, Weaker, EAllowShrinking::No);
		Bytes -= Estimate(Removed);
		SeenIds.Remove(GetId(Removed));
		bTruncated = true;
	}
}
