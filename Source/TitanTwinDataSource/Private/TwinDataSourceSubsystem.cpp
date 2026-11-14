// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinDataSourceSubsystem.h"

#include "TitanTwinDataSource.h"
#include "TwinDataSourceMapping.h"
#include "TwinEntityComponent.h"
#include "TwinWorldSubsystem.h"
#include "Engine/AssetManager.h"
#include "Engine/World.h"
#include "Engine/StreamableManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"
#if WITH_EDITOR
#include "UObject/SavePackage.h"
#endif

bool UTwinDataSourceSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::GamePreview;
}

void UTwinDataSourceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UTwinWorldSubsystem>();
	Super::Initialize(Collection);

	const UTwinDataSourceSettings* Settings = GetDefault<UTwinDataSourceSettings>();
	Ingress = MakeShared<FTwinDataSourceIngress, ESPMode::ThreadSafe>(Settings->MaxPendingPoints, Settings->MaxPendingBytes);
	FString ConfigurationError;
	if (!Settings->Validate(ConfigurationError))
	{
		Ingress->StopAccepting();
		UE_LOG(LogTitanTwinDataSource, Error, TEXT("%s"), *ConfigurationError);
		return;
	}
	if (UTwinWorldSubsystem* WorldSubsystem = GetWorld()->GetSubsystem<UTwinWorldSubsystem>())
	{
		WorldSubsystem->OnEntityRegisteredNative.AddUObject(
			this,
			&UTwinDataSourceSubsystem::HandleEntityRegistered);
		WorldSubsystem->OnEntityUnregisteredNative.AddUObject(this, &UTwinDataSourceSubsystem::HandleEntityUnregistered);
		for (auto* Entity : WorldSubsystem->GetAllEntities()) HandleEntityRegistered(Entity);
	}
	ReloadConfiguredMappings();
	StartPumpTimer();
}

void UTwinDataSourceSubsystem::Deinitialize()
{
	FlushAutomaticMappingSaves(true);
	StopPumpTimer();
	if (UWorld* World = GetWorld())
	{
		if (UTwinWorldSubsystem* WorldSubsystem = World->GetSubsystem<UTwinWorldSubsystem>())
		{
			WorldSubsystem->OnEntityRegisteredNative.RemoveAll(this);
			WorldSubsystem->OnEntityUnregisteredNative.RemoveAll(this);
		}
	}
	for (auto Weak : ObservedEntities) if (auto* Entity = Weak.Get()) Entity->OnEntityIdChangedNative.RemoveAll(this);
	ObservedEntities.Reset();
	if (Ingress.IsValid())
	{
		Ingress->StopAccepting();
		Ingress.Reset();
	}
	if (MappingLoadHandle.IsValid())
	{
		MappingLoadHandle->CancelHandle();
		MappingLoadHandle.Reset();
	}

	ExactMappings.Empty();
	WildcardMappings.Empty();
	MappingSignatures.Empty();
	MappingTargetSignatures.Empty();
	SourceStatuses.Empty();
	PendingEntitySamples.Empty(); PendingEntityBytes = 0;
	OnSourceStateChanged.Clear();
	Super::Deinitialize();
}

bool UTwinDataSourceSubsystem::SubmitDataPoint(const FTwinDataPoint& Point)
{
	return Ingress.IsValid() && Ingress->Enqueue(Point);
}

int32 UTwinDataSourceSubsystem::PumpNow()
{
	if (!ensureMsgf(IsInGameThread(), TEXT("Twin data source pump must run on the game thread."))
		|| !Ingress.IsValid())
	{
		return 0;
	}

	const UTwinDataSourceSettings* Settings = GetDefault<UTwinDataSourceSettings>();
	const int32 MaxPoints = FMath::Max(1, Settings->MaxPointsPerPump);
	FString ConfigurationError;
	if (!Settings->Validate(ConfigurationError)) return 0;
	const double TimeBudgetSeconds = FMath::Max(0.0f, Settings->PumpTimeBudgetMilliseconds) / 1000.0;
	const double StartSeconds = FPlatformTime::Seconds();
	int32 ProcessedPoints = 0;
	FTwinDataPoint Point;
	while (ProcessedPoints < MaxPoints)
	{
		if (ProcessedPoints > 0
			&& TimeBudgetSeconds > 0.0
			&& FPlatformTime::Seconds() - StartSeconds >= TimeBudgetSeconds)
		{
			break;
		}
		if (!Ingress->Dequeue(Point))
		{
			break;
		}

		++ProcessedPoints;
		FString Reason;
		if (!Point.IsValid(&Reason))
		{
			++InvalidPoints;
			UE_LOG(LogTitanTwinDataSource, Warning, TEXT("Rejected data point: %s"), *Reason);
			continue;
		}

		++AcceptedPoints;
		AppliedSamples += ApplyPoint(Point);
	}

	PruneExpiredPendingSamples(FPlatformTime::Seconds());
	FlushAutomaticMappingSaves();
	return ProcessedPoints;
}

bool UTwinDataSourceSubsystem::RegisterMapping(const FTwinDataSourceMapping& Mapping, FString& OutError)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("RegisterMapping must run on the game thread.")))
	{
		OutError = TEXT("RegisterMapping must run on the game thread.");
		return false;
	}
	if (!Mapping.IsValid(&OutError))
	{
		return false;
	}
	if (!Mapping.bEnabled)
	{
		OutError = TEXT("Mapping is disabled.");
		return false;
	}
	const FString Signature = MakeMappingSignature(Mapping);
	if (MappingSignatures.Contains(Signature))
	{
		OutError = TEXT("An identical mapping is already registered.");
		return false;
	}
	const FString TargetSignature = MakeMappingTargetSignature(Mapping);
	if (MappingTargetSignatures.Contains(TargetSignature))
	{
		OutError = TEXT("A mapping already targets the same source, address, entity, and property.");
		return false;
	}

	AddMappingToIndex(Mapping);
	MappingSignatures.Add(Signature);
	MappingTargetSignatures.Add(TargetSignature);
	++RegisteredMappingCount;
	OutError.Reset();
	return true;
}

int32 UTwinDataSourceSubsystem::RegisterMappingAsset(const UTwinDataSourceMappingAsset* MappingAsset)
{
	if (!IsValid(MappingAsset))
	{
		return 0;
	}

	int32 Added = 0;
	for (const FTwinDataSourceMapping& Mapping : MappingAsset->Mappings)
	{
		FString Error;
		if (RegisterMapping(Mapping, Error))
		{
			++Added;
		}
		else if (Mapping.bEnabled)
		{
			UE_LOG(
				LogTitanTwinDataSource,
				Warning,
				TEXT("Mapping asset '%s' contains an invalid mapping: %s"),
				*GetNameSafe(MappingAsset),
				*Error);
		}
	}
	return Added;
}

void UTwinDataSourceSubsystem::ClearMappings()
{
	RuntimeRuleAddresses.Empty();
	ExactMappings.Empty();
	WildcardMappings.Empty();
	MappingSignatures.Empty();
	MappingTargetSignatures.Empty();
	PendingEntitySamples.Empty(); PendingEntityBytes = 0;
	RegisteredMappingCount = 0;
}

void UTwinDataSourceSubsystem::ReloadConfiguredMappings()
{
	if (MappingLoadHandle.IsValid())
	{
		MappingLoadHandle->CancelHandle();
		MappingLoadHandle.Reset();
	}

	ClearMappings();

	const UTwinDataSourceSettings* Settings =
		GetDefault<UTwinDataSourceSettings>();

	if (!Settings)
	{
		UE_LOG(
			LogTitanTwinDataSource,
			Error,
			TEXT("Titan Twin Data Source settings are unavailable."));
		return;
	}

	for (const TSoftObjectPtr<UTwinDataSourceMappingAsset>& AssetReference
		: Settings->MappingAssets)
	{
		if (AssetReference.IsNull())
		{
			continue;
		}

		UTwinDataSourceMappingAsset* Asset =
			AssetReference.LoadSynchronous();

		if (!IsValid(Asset))
		{
			UE_LOG(
				LogTitanTwinDataSource,
				Error,
				TEXT("Could not synchronously load mapping asset '%s'."),
				*AssetReference.ToString());
			continue;
		}

		const int32 AddedMappings = RegisterMappingAsset(Asset);

		UE_LOG(
			LogTitanTwinDataSource,
			Display,
			TEXT("Loaded mapping asset '%s': %d mapping(s) registered."),
			*GetNameSafe(Asset),
			AddedMappings);
	}

	UE_LOG(
		LogTitanTwinDataSource,
		Display,
		TEXT("Configured mapping load complete. RegisteredMappings=%d"),
		RegisteredMappingCount);
}
void UTwinDataSourceSubsystem::FinishConfiguredMappingLoad()
{
	MappingLoadHandle.Reset();
	const UTwinDataSourceSettings* Settings = GetDefault<UTwinDataSourceSettings>();
	for (const TSoftObjectPtr<UTwinDataSourceMappingAsset>& AssetReference : Settings->MappingAssets)
	{
		if (UTwinDataSourceMappingAsset* Asset = AssetReference.Get())
		{
			RegisterMappingAsset(Asset);
		}
		else if (!AssetReference.IsNull())
		{
			UE_LOG(
				LogTitanTwinDataSource,
				Warning,
				TEXT("Could not load configured mapping asset '%s'."),
				*AssetReference.ToString());
		}
	}
}

void UTwinDataSourceSubsystem::ReportSourceState(
	FName SourceId,
	ETwinDataSourceState NewState,
	const FString& ErrorMessage)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("ReportSourceState must run on the game thread."))
		|| SourceId.IsNone())
	{
		return;
	}

	FTwinDataSourceStatus& Status = SourceStatuses.FindOrAdd(SourceId);
	const bool bChanged = Status.SourceId.IsNone()
		|| Status.State != NewState
		|| Status.LastError != ErrorMessage;
	Status.SourceId = SourceId;
	Status.State = NewState;
	Status.LastError = ErrorMessage;
	if (bChanged)
	{
		Status.LastChangedUtc = FDateTime::UtcNow();
		OnSourceStateChanged.Broadcast(SourceId, NewState);
	}
}

bool UTwinDataSourceSubsystem::GetSourceStatus(FName SourceId, FTwinDataSourceStatus& OutStatus) const
{
	if (const FTwinDataSourceStatus* Status = SourceStatuses.Find(SourceId))
	{
		OutStatus = *Status;
		return true;
	}
	return false;
}

TArray<FTwinDataSourceStatus> UTwinDataSourceSubsystem::GetAllSourceStatuses() const
{
	TArray<FTwinDataSourceStatus> Result;
	SourceStatuses.GenerateValueArray(Result);
	Result.Sort([](const FTwinDataSourceStatus& A, const FTwinDataSourceStatus& B)
	{
		return A.SourceId.LexicalLess(B.SourceId);
	});
	return Result;
}

FTwinDataSourceMetrics UTwinDataSourceSubsystem::GetMetrics() const
{
	FTwinDataSourceMetrics Result;
	Result.AcceptedPoints = AcceptedPoints;
	Result.AppliedSamples = AppliedSamples;
	Result.InvalidPoints = InvalidPoints;
	Result.UnmappedPoints = UnmappedPoints;
	Result.ClampedFutureTimestamps = ClampedFutureTimestamps;
	Result.MissingEntityTargets = MissingEntityTargets;
	Result.DroppedByBackpressure = Ingress.IsValid() ? Ingress->GetDroppedCount() : 0;
	Result.PendingPoints = Ingress.IsValid() ? Ingress->GetPendingCount() : 0;
	Result.PendingBytes = Ingress.IsValid() ? Ingress->GetPendingBytes() : 0;
	Result.InvalidPoints += Ingress.IsValid() ? Ingress->GetInvalidCount() : 0;
	Result.RegisteredMappings = RegisteredMappingCount;
	Result.PendingEntitySamples = PendingEntitySamples.Num();
	Result.PendingEntityBytes = PendingEntityBytes;
	Result.PeakPendingEntityBytes = PeakPendingEntityBytes;
	Result.ReplayedPendingEntitySamples = ReplayedPendingEntitySamples;
	Result.ExpiredPendingEntitySamples = ExpiredPendingEntitySamples;
	Result.DroppedPendingEntitySamples = DroppedPendingEntitySamples;
	return Result;
}

TSharedPtr<FTwinDataSourceIngress, ESPMode::ThreadSafe> UTwinDataSourceSubsystem::GetIngress() const
{
	return Ingress;
}

FString UTwinDataSourceSubsystem::MakeMappingKey(FName SourceId, const FString& Address)
{
	return SourceId.ToString() + TEXT("\x1f") + Address.TrimStartAndEnd();
}

FString UTwinDataSourceSubsystem::MakeMappingSignature(const FTwinDataSourceMapping& Mapping)
{
	return MakeMappingTargetSignature(Mapping)
		+ TEXT("\x1f") + LexToString(Mapping.bOverrideUnit)
		+ TEXT("\x1f") + Mapping.Unit.ToString()
		+ TEXT("\x1f") + LexToString(Mapping.bEnabled);
}

FString UTwinDataSourceSubsystem::MakeMappingTargetSignature(const FTwinDataSourceMapping& Mapping)
{
	return MakeMappingKey(Mapping.SourceId, Mapping.Address)
		+ TEXT("\x1f") + Mapping.EntityId.ToString()
		+ TEXT("\x1f") + Mapping.PropertyName.ToString();
}

FString UTwinDataSourceSubsystem::MakePendingSampleKey(FName EntityId, FName PropertyName)
{
	return EntityId.ToString() + TEXT("\x1f") + PropertyName.ToString();
}

void UTwinDataSourceSubsystem::StartPumpTimer()
{
	if (UWorld* World = GetWorld())
	{
		const float Interval = GetDefault<UTwinDataSourceSettings>()->PumpIntervalSeconds;
		FTimerManagerTimerParameters Parameters;
		Parameters.bLoop = true;
		Parameters.bMaxOncePerFrame = true;
		World->GetTimerManager().SetTimer(
			PumpTimerHandle,
			this,
			&UTwinDataSourceSubsystem::HandlePumpTimer,
			FMath::Max(0.001f, Interval),
			Parameters);
	}
}

void UTwinDataSourceSubsystem::StopPumpTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PumpTimerHandle);
	}
}

void UTwinDataSourceSubsystem::HandlePumpTimer()
{
	PumpNow();
}

void UTwinDataSourceSubsystem::HandleEntityRegistered(UTwinEntityComponent* Entity)
{
	if (!IsValid(Entity)) return;
	Entity->OnEntityIdChangedNative.RemoveAll(this);
	Entity->OnEntityIdChangedNative.AddUObject(this, &UTwinDataSourceSubsystem::HandleEntityIdChanged);
	ObservedEntities.AddUnique(Entity);
	ReplayPendingSamples(Entity);
}

void UTwinDataSourceSubsystem::HandleEntityUnregistered(UTwinEntityComponent* Entity)
{
	if (Entity) Entity->OnEntityIdChangedNative.RemoveAll(this);
	ObservedEntities.Remove(Entity);
}

void UTwinDataSourceSubsystem::HandleEntityIdChanged(UTwinEntityComponent* Entity, FName Previous, FName Current)
{
	if (IsValid(Entity) && Entity->IsRegistered() && Entity->GetEntityId() == Current) ReplayPendingSamples(Entity);
}

int32 UTwinDataSourceSubsystem::ApplyPoint(const FTwinDataPoint& Point)
{
	TArray<FTwinDataSourceMapping, TInlineAllocator<1>> Targets;
	if (const TArray<FTwinDataSourceMapping>* Exact = ExactMappings.Find(MakeMappingKey(Point.SourceId, Point.Address)))
	{
		Targets.Append(*Exact);
	}
	else if (const TArray<FTwinDataSourceMapping>* Wildcard = WildcardMappings.Find(Point.Address.TrimStartAndEnd()))
	{
		Targets.Append(*Wildcard);
	}
	else
	{
		FTwinDataSourceMapping AutomaticMapping;
		if (TryCreateAndSaveAutomaticMapping(Point, AutomaticMapping))
		{
			Targets.Add(MoveTemp(AutomaticMapping));
		}
        else if (CanResolveRuntimeRule(Point, AutomaticMapping))
        {
            RuntimeRuleAddresses.Add(MakeMappingKey(Point.SourceId,Point.Address));
            Targets.Add(MoveTemp(AutomaticMapping));
        }
	}

	if (Targets.IsEmpty())
	{
		++UnmappedPoints;
		return 0;
	}

	UTwinWorldSubsystem* WorldSubsystem = GetWorld()->GetSubsystem<UTwinWorldSubsystem>();
	if (!WorldSubsystem)
	{
		MissingEntityTargets += Targets.Num();
		return 0;
	}

	// 设备时钟偏快或发来不带时区的本地时间时，时间戳会落在未来。
	// 不能让它推进实体的时序水位，所以改用接收时间；数据本身照常写入。
	const FDateTime NowUtc = FDateTime::UtcNow();
	FDateTime SampleTimestamp = Point.TimestampUtc.GetTicks() > 0 ? Point.TimestampUtc : NowUtc;
	if (SampleTimestamp > NowUtc + FTimespan::FromSeconds(TwinDataSourceLimits::MaximumFutureClockSkewSeconds))
	{
		++ClampedFutureTimestamps;
		const double NowSeconds = FPlatformTime::Seconds();
		if (LastFutureTimestampWarningSeconds <= 0.0 || NowSeconds - LastFutureTimestampWarningSeconds >= 10.0)
		{
			LastFutureTimestampWarningSeconds = NowSeconds;
			UE_LOG(
				LogTitanTwinDataSource,
				Warning,
				TEXT("Data point '%s' from source '%s' is timestamped %s, %.0f s ahead of local UTC; using receive time instead. Total clamped: %lld. Check the device clock or time zone."),
				*Point.Address,
				*Point.SourceId.ToString(),
				*SampleTimestamp.ToIso8601(),
				(SampleTimestamp - NowUtc).GetTotalSeconds(),
				ClampedFutureTimestamps);
		}
		SampleTimestamp = NowUtc;
	}

	int32 Applied = 0;
	for (const FTwinDataSourceMapping& Mapping : Targets)
	{
		FTwinPropertySample Sample;
		Sample.Value = Point.Value;
		Sample.Timestamp = SampleTimestamp;
		Sample.Quality = Point.Quality;
		Sample.Unit = GetDefault<UTwinDataSourceSettings>()->ResolveUnit(
			Mapping.PropertyName,
			Point.Unit,
			Mapping.bOverrideUnit,
			Mapping.Unit);
		Sample.Source = Point.SourceId.IsNone() ? FName(TEXT("DataSource")) : Point.SourceId;

		UTwinEntityComponent* Entity = WorldSubsystem->FindEntity(Mapping.EntityId);
		if (!Entity)
		{
			++MissingEntityTargets;
			BufferPendingSample(Mapping.EntityId, Mapping.PropertyName, Sample);
			continue;
		}

		if (ApplySample(Entity, Mapping.PropertyName, Sample))
		{
			++Applied;
		}
	}
	return Applied;
}

bool UTwinDataSourceSubsystem::ApplySample(
	UTwinEntityComponent* Entity,
	FName PropertyName,
	const FTwinPropertySample& Sample)
{
	if (!IsValid(Entity))
	{
		return false;
	}

	FTwinPropertySample ExistingSample;
	const FDateTime LatestTrustedTimestamp = FDateTime::UtcNow()
		+ FTimespan::FromSeconds(TwinDataSourceLimits::MaximumFutureClockSkewSeconds);
	if (Entity->GetLivePropertySample(PropertyName, ExistingSample)
		&& ExistingSample.Timestamp.GetTicks() > 0
		&& ExistingSample.Timestamp <= LatestTrustedTimestamp
		&& Sample.Timestamp.GetTicks() > 0
		&& Sample.Timestamp < ExistingSample.Timestamp)
	{
		return false;
	}

	if (!Entity->SetPropertySample(PropertyName, Sample))
	{
		return false;
	}

	Entity->SetConnectionState(ETwinConnectionState::Online);
	return true;
}

void UTwinDataSourceSubsystem::BufferPendingSample(
	FName EntityId,
	FName PropertyName,
	const FTwinPropertySample& Sample)
{
	const UTwinDataSourceSettings* Settings = GetDefault<UTwinDataSourceSettings>();
	if (!Settings->bBufferMissingEntitySamples)
	{
		return;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	PruneExpiredPendingSamples(NowSeconds);
	const FString Key = MakePendingSampleKey(EntityId, PropertyName);
	const int64 Bytes = sizeof(FPendingEntitySample) + Key.GetAllocatedSize() + Sample.Value.TextValue.GetAllocatedSize();
	const int64 Budget = Settings->MaxPendingEntityBytes;
	if (FPendingEntitySample* Existing = PendingEntitySamples.Find(Key))
	{
		if (Existing->Sample.Timestamp.GetTicks() > 0
			&& Existing->Sample.Timestamp <= FDateTime::UtcNow()
				+ FTimespan::FromSeconds(TwinDataSourceLimits::MaximumFutureClockSkewSeconds)
			&& Sample.Timestamp.GetTicks() > 0
			&& Sample.Timestamp < Existing->Sample.Timestamp)
		{
			return;
		}

		if (Bytes > Budget - (PendingEntityBytes - Existing->Bytes)) { ++DroppedPendingEntitySamples; return; }
		PendingEntityBytes += Bytes - Existing->Bytes;
		Existing->Bytes = Bytes;
		PeakPendingEntityBytes = FMath::Max(PeakPendingEntityBytes, PendingEntityBytes);
		Existing->Sample = Sample;
		Existing->BufferedAtSeconds = NowSeconds;
		return;
	}

	const int32 Capacity = FMath::Max(1, Settings->MaxPendingEntitySamples);
	if (Bytes > Budget) { ++DroppedPendingEntitySamples; return; }
	while (PendingEntitySamples.Num() >= Capacity || Bytes > Budget - PendingEntityBytes)
	{
		FString OldestKey;
		double OldestTime = TNumericLimits<double>::Max();
		for (const TPair<FString, FPendingEntitySample>& Pair : PendingEntitySamples)
		{
			if (Pair.Value.BufferedAtSeconds < OldestTime)
			{
				OldestTime = Pair.Value.BufferedAtSeconds;
				OldestKey = Pair.Key;
			}
		}
		if (!OldestKey.IsEmpty())
		{
			PendingEntityBytes -= PendingEntitySamples.FindChecked(OldestKey).Bytes;
			PendingEntitySamples.Remove(OldestKey);
			++DroppedPendingEntitySamples;
		}
	}

	FPendingEntitySample& Pending = PendingEntitySamples.Add(Key);
	Pending.EntityId = EntityId;
	Pending.PropertyName = PropertyName;
	Pending.Sample = Sample;
	Pending.BufferedAtSeconds = NowSeconds;
	Pending.Bytes = Bytes; PendingEntityBytes += Bytes;
	PeakPendingEntityBytes = FMath::Max(PeakPendingEntityBytes, PendingEntityBytes);
}

void UTwinDataSourceSubsystem::ReplayPendingSamples(UTwinEntityComponent* Entity)
{
	if (!IsValid(Entity))
	{
		return;
	}

	PruneExpiredPendingSamples(FPlatformTime::Seconds(), true);
	const FName EntityId = Entity->GetEntityId();
	TArray<FString> MatchingKeys;
	for (const TPair<FString, FPendingEntitySample>& Pair : PendingEntitySamples)
	{
		if (Pair.Value.EntityId == EntityId)
		{
			MatchingKeys.Add(Pair.Key);
		}
	}

	for (const FString& Key : MatchingKeys)
	{
		FPendingEntitySample Pending;
		if (!PendingEntitySamples.RemoveAndCopyValue(Key, Pending))
		{
			continue;
		}

		PendingEntityBytes -= Pending.Bytes;
		if (ApplySample(Entity, Pending.PropertyName, Pending.Sample))
		{
			++AppliedSamples;
			++ReplayedPendingEntitySamples;
		}
	}
}

void UTwinDataSourceSubsystem::PruneExpiredPendingSamples(double NowSeconds, bool bForce)
{
	if (!bForce && NowSeconds - LastPendingSamplePruneAtSeconds < 1.0)
	{
		return;
	}
	LastPendingSamplePruneAtSeconds = NowSeconds;

	const double TtlSeconds = FMath::Max(
		0.1f,
		GetDefault<UTwinDataSourceSettings>()->PendingEntitySampleTtlSeconds);
	for (auto Iterator = PendingEntitySamples.CreateIterator(); Iterator; ++Iterator)
	{
		if (NowSeconds - Iterator.Value().BufferedAtSeconds > TtlSeconds)
		{
			PendingEntityBytes -= Iterator.Value().Bytes;
			Iterator.RemoveCurrent();
			++ExpiredPendingEntitySamples;
		}
	}
}

bool UTwinDataSourceSubsystem::TryCreateAndSaveAutomaticMapping(
	const FTwinDataPoint& Point, FTwinDataSourceMapping& OutMapping)
{
#if WITH_EDITOR
	UTwinDataSourceMappingAsset* Asset = nullptr;
	if (!CanCreateAutomaticMapping(Point, OutMapping, Asset)) return false;
	FString Error;
	if (!RegisterMapping(OutMapping, Error)) return false;
	Asset->Modify();
	Asset->Mappings.Add(OutMapping);
	Asset->MarkPackageDirty();
	PendingMappingAssets.AddUnique(Asset);
	return true;
#else
	return false;
#endif
}

bool UTwinDataSourceSubsystem::CanCreateAutomaticMapping(
	const FTwinDataPoint& Point, FTwinDataSourceMapping& OutMapping,
	UTwinDataSourceMappingAsset*& OutAsset) const
{
	OutAsset = nullptr;
#if WITH_EDITOR
	const auto* Settings = GetDefault<UTwinDataSourceSettings>();
	if (!Settings || !Settings->bEnableAutomaticMapping
		|| !Settings->TryBuildAutomaticMapping(Point, OutMapping)) return false;
	TSoftObjectPtr<UTwinDataSourceMappingAsset> Reference = Settings->AutomaticMappingAsset;
	if (Reference.IsNull())
		for (const auto& Configured : Settings->MappingAssets)
			if (!Configured.IsNull()) { Reference = Configured; break; }
	OutAsset = Reference.LoadSynchronous();
	if (!IsValid(OutAsset) || !OutAsset->GetOutermost()
		|| OutAsset->Mappings.Num() >= Settings->MaxAutomaticMappings
		|| MappingSignatures.Contains(MakeMappingSignature(OutMapping))
		|| MappingTargetSignatures.Contains(MakeMappingTargetSignature(OutMapping))) return false;
	return !OutAsset->Mappings.ContainsByPredicate([&](const FTwinDataSourceMapping& Existing)
		{ return Existing.SourceId == OutMapping.SourceId
			&& Existing.Address.TrimStartAndEnd() == OutMapping.Address; });
#else
	return false;
#endif
}

void UTwinDataSourceSubsystem::FlushAutomaticMappingSaves(bool bForce)
{
#if WITH_EDITOR
	if (PendingMappingAssets.IsEmpty()) return;
	const double Now = FPlatformTime::Seconds();
	const double Interval = FMath::Clamp(GetDefault<UTwinDataSourceSettings>()->AutomaticMappingSaveIntervalSeconds, 0.1f, 60.0f);
	if (!bForce && Now - LastAutomaticMappingSaveSeconds < Interval) return;
	LastAutomaticMappingSaveSeconds = Now;
	TArray<TObjectPtr<UTwinDataSourceMappingAsset>> Save = MoveTemp(PendingMappingAssets);
	for (UTwinDataSourceMappingAsset* Asset : Save)
	{
		if (!IsValid(Asset) || !Asset->GetOutermost()) continue;
		UPackage* Package = Asset->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Asset, *Filename, Args))
		{
			PendingMappingAssets.AddUnique(Asset);
			UE_LOG(LogTitanTwinDataSource, Error, TEXT("Automatic mapping save failed; the dirty asset is retained for retry."));
		}
	}
#endif
}

bool UTwinDataSourceSubsystem::PreviewDataPoint(
	const FTwinDataPoint& Point, TArray<FTwinDataSourceMapping>& OutMappings, FString& OutError) const
{
	OutMappings.Reset(); OutError.Reset();
	if (!IsInGameThread()) { OutError = TEXT("Mapping preview must run on the game thread."); return false; }
	if (!Point.IsValid(&OutError)) return false;
	if (const auto* Exact = ExactMappings.Find(MakeMappingKey(Point.SourceId, Point.Address))) OutMappings = *Exact;
	else if (const auto* Wildcard = WildcardMappings.Find(Point.Address.TrimStartAndEnd())) OutMappings = *Wildcard;
	else
	{
		FTwinDataSourceMapping Automatic;
		UTwinDataSourceMappingAsset* Asset = nullptr;
		if (CanCreateAutomaticMapping(Point, Automatic, Asset) || CanResolveRuntimeRule(Point, Automatic))
			OutMappings.Add(MoveTemp(Automatic));
	}
	if (OutMappings.IsEmpty()) { OutError = TEXT("No executable Mapping. Configure an explicit mapping, editor learning asset, or enabled runtime address rules."); return false; }
	return true;
}

bool UTwinDataSourceSubsystem::PreviewRegisteredDataPoint(
	const FTwinDataPoint& Point, TArray<FTwinDataSourceMapping>& OutMappings, FString& OutError) const
{
	OutMappings.Reset(); OutError.Reset();
	if (!IsInGameThread()) { OutError = TEXT("Mapping preview must run on the game thread."); return false; }
	if (!Point.IsValid(&OutError)) return false;
	if (const auto* Exact = ExactMappings.Find(MakeMappingKey(Point.SourceId, Point.Address))) OutMappings = *Exact;
	else if (const auto* Wildcard = WildcardMappings.Find(Point.Address.TrimStartAndEnd())) OutMappings = *Wildcard;
	if (OutMappings.IsEmpty()) { OutError = TEXT("Address has no registered Mapping. Automatic rule matches are not an existing Mapping."); return false; }
	return true;
}

void UTwinDataSourceSubsystem::AddMappingToIndex(const FTwinDataSourceMapping& Mapping)
{
	if (Mapping.SourceId.IsNone())
	{
		WildcardMappings.FindOrAdd(Mapping.Address.TrimStartAndEnd()).Add(Mapping);
	}
	else
	{
		ExactMappings.FindOrAdd(MakeMappingKey(Mapping.SourceId, Mapping.Address)).Add(Mapping);
	}
}

bool UTwinDataSourceSubsystem::CanResolveRuntimeRule(const FTwinDataPoint& Point, FTwinDataSourceMapping& OutMapping) const
{
    const auto* Settings=GetDefault<UTwinDataSourceSettings>();
    if (!Settings->bEnableRuntimeRuleMapping || Settings->MaxRuntimeRuleAddresses<1
        || Settings->MaxRuntimeRuleAddresses>65536) return false;
    const FString Key=MakeMappingKey(Point.SourceId,Point.Address);
    if (!RuntimeRuleAddresses.Contains(Key) && RuntimeRuleAddresses.Num()>=Settings->MaxRuntimeRuleAddresses) return false;
    return Settings->TryBuildAutomaticMapping(Point,OutMapping,false);
}
