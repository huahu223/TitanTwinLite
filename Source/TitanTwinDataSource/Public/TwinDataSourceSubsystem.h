// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TwinDataSourceIngress.h"
#include "TwinDataSourceTypes.h"
#include "TwinDataSourceSubsystem.generated.h"

class UTwinDataSourceMappingAsset;
class UTwinEntityComponent;
struct FStreamableHandle;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinDataSourceStateChangedSignature,
	FName,
	SourceId,
	ETwinDataSourceState,
	NewState);

/**
 * 外部数据的 Mapping 和写入都在游戏线程处理。
 * 工作线程必须调用 GetIngress()。调用 StopAccepting() 后，
 * 后面再来的数据不会继续入队。
 */
UCLASS()
class TITANTWINDATASOURCE_API UTwinDataSourceSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

/** 数据源运行状态实际变化后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Data Source")
	FTwinDataSourceStateChangedSignature OnSourceStateChanged;

	// 蓝图或游戏线程从这里传数据，原生协议适配器用 GetIngress()。
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source")
	bool SubmitDataPoint(const FTwinDataPoint& Point);

/**
 * 立即消费当前队列，返回本次处理的数据点数量。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source")
	int32 PumpNow();

/**
 * 注册显式 Mapping，并在失败时返回具体校验原因。
 * 同一来源和地址可以注册多个不同目标；完全相同的目标会被拒绝，现有项不会被覆盖。
 * @param Mapping 要注册的地址到实体属性映射。
 * @param OutError 失败时接收可显示的原因。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source")
	bool RegisterMapping(const FTwinDataSourceMapping& Mapping, FString& OutError);

	/** Resolve a sample without writing properties, creating assets or persisting mappings. */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source|Preview")
	bool PreviewDataPoint(const FTwinDataPoint& Point, TArray<FTwinDataSourceMapping>& OutMappings, FString& OutError) const;

	/** Strict preflight for adapters that require an already registered Mapping. Never learns an address. */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source|Preview")
	bool PreviewRegisteredDataPoint(const FTwinDataPoint& Point, TArray<FTwinDataSourceMapping>& OutMappings, FString& OutError) const;

/**
 * 批量注册资产中启用的 Mapping，返回成功数量。
 * @param MappingAsset 包含一组映射的配置资产。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source")
	int32 RegisterMappingAsset(const UTwinDataSourceMappingAsset* MappingAsset);

/** 清空运行时 Mapping，不修改资产和项目设置。 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source")
	void ClearMappings();

/** 重新加载设置中的 Mapping 资产和自动规则。 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source")
	void ReloadConfiguredMappings();

/**
 * 更新数据源状态；只有实际变化时才广播事件。
 * @param SourceId 产生数据或状态的来源标识。
 * @param NewState 准备写入的新状态。
 * @param ErrorMessage 需要保存或显示的失败原因。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Data Source")
	void ReportSourceState(FName SourceId, ETwinDataSourceState NewState, const FString& ErrorMessage);

/**
 * 复制指定数据源的最近状态，未见过该来源时返回 false。
 * @param SourceId 产生数据或状态的来源标识。
 * @param OutStatus 找到后接收状态副本。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Data Source")
	bool GetSourceStatus(FName SourceId, FTwinDataSourceStatus& OutStatus) const;

/**
 * 返回所有已知数据源状态的快照。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Data Source")
	TArray<FTwinDataSourceStatus> GetAllSourceStatuses() const;

/**
 * 查看队列、Mapping 命中和丢弃情况。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Data Source")
	FTwinDataSourceMetrics GetMetrics() const;

	// 这是给 MQTT、HTTP、OPC UA 适配插件使用的线程安全入口。
	TSharedPtr<FTwinDataSourceIngress, ESPMode::ThreadSafe> GetIngress() const;

private:
	void FlushAutomaticMappingSaves(bool bForce = false);
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTwinDataSourceMappingAsset>> PendingMappingAssets;
	double LastAutomaticMappingSaveSeconds = 0.0;
	struct FPendingEntitySample
	{
		FName EntityId;
		FName PropertyName;
		FTwinPropertySample Sample;
		double BufferedAtSeconds = 0.0;
		int64 Bytes = 0;
	};

	static FString MakeMappingKey(FName SourceId, const FString& Address);
	static FString MakeMappingSignature(const FTwinDataSourceMapping& Mapping);
	static FString MakeMappingTargetSignature(const FTwinDataSourceMapping& Mapping);
	static FString MakePendingSampleKey(FName EntityId, FName PropertyName);
	void FinishConfiguredMappingLoad();
	void StartPumpTimer();
	void StopPumpTimer();
	void HandlePumpTimer();
	void HandleEntityRegistered(UTwinEntityComponent* Entity);
	void HandleEntityUnregistered(UTwinEntityComponent* Entity);
	void HandleEntityIdChanged(UTwinEntityComponent* Entity, FName Previous, FName Current);
	TArray<TWeakObjectPtr<UTwinEntityComponent>> ObservedEntities;
	int64 PendingEntityBytes = 0;
	int64 PeakPendingEntityBytes = 0;
	int32 ApplyPoint(const FTwinDataPoint& Point);
	bool ApplySample(UTwinEntityComponent* Entity, FName PropertyName, const FTwinPropertySample& Sample);
	void BufferPendingSample(FName EntityId, FName PropertyName, const FTwinPropertySample& Sample);
	void ReplayPendingSamples(UTwinEntityComponent* Entity);
	void PruneExpiredPendingSamples(double NowSeconds, bool bForce = false);
	void AddMappingToIndex(const FTwinDataSourceMapping& Mapping);
	bool TryCreateAndSaveAutomaticMapping(const FTwinDataPoint& Point, FTwinDataSourceMapping& OutMapping);
	bool CanCreateAutomaticMapping(const FTwinDataPoint& Point, FTwinDataSourceMapping& OutMapping,
		UTwinDataSourceMappingAsset*& OutAsset) const;

	TSharedPtr<FTwinDataSourceIngress, ESPMode::ThreadSafe> Ingress;
	TMap<FString, TArray<FTwinDataSourceMapping>> ExactMappings;
	TMap<FString, TArray<FTwinDataSourceMapping>> WildcardMappings;
    TSet<FString> RuntimeRuleAddresses;
    bool CanResolveRuntimeRule(const FTwinDataPoint& Point, FTwinDataSourceMapping& OutMapping) const;
	TSet<FString> MappingSignatures;
	TSet<FString> MappingTargetSignatures;
	TSharedPtr<FStreamableHandle> MappingLoadHandle;
	TMap<FName, FTwinDataSourceStatus> SourceStatuses;
	TMap<FString, FPendingEntitySample> PendingEntitySamples;
	FTimerHandle PumpTimerHandle;
	double LastPendingSamplePruneAtSeconds = 0.0;

	int64 AcceptedPoints = 0;
	int64 AppliedSamples = 0;
	int64 InvalidPoints = 0;
	int64 UnmappedPoints = 0;
	int64 ClampedFutureTimestamps = 0;
	double LastFutureTimestampWarningSeconds = 0.0;
	int64 MissingEntityTargets = 0;
	int64 ReplayedPendingEntitySamples = 0;
	int64 ExpiredPendingEntitySamples = 0;
	int64 DroppedPendingEntitySamples = 0;
	int32 RegisteredMappingCount = 0;
};
