// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DeveloperSettings.h"
#include "TwinDataSourceTypes.h"
#include "TwinDataSourceMapping.generated.h"

/**
 * 这是一条可以配置的地址模板，用地址推断实体和属性。
 * 模板里必须各有一个 {entity} 和 {property} 占位符。
 * 其他命名占位符只负责匹配一个非空值，匹配完不用保存。
 */
USTRUCT(BlueprintType)
struct TITANTWINDATASOURCE_API FTwinAutomaticMappingRule
{
	GENERATED_BODY()

/** 这条自动规则是否参加地址匹配。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Automatic Mapping")
	bool bEnabled = true;

	/** 这里选要匹配的数据源。填 None 就匹配所有数据源。 */
	UPROPERTY(
		Config,
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Automatic Mapping",
		meta = (DisplayName = "Source Id (None = Any)"))
	FName SourceId;

	/**
	 * 地址模板可以这样写：factory/{entity}/{property}，
	 * 也可以写成 site/{site}/line/{line}/{entity}/{property} 或 ns=2;s={entity}/{property}。
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Automatic Mapping")
	FString AddressTemplate = TEXT("factory/{entity}/{property}");

	/** MQTT Topic 区分大小写，所以默认开启。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Automatic Mapping")
	bool bCaseSensitive = true;
};

/**
 * 可复用的数据源 Mapping 资产。
 * 运行时会复制启用项，修改资产不会直接改动当前连接。
 */
UCLASS(BlueprintType)
class TITANTWINDATASOURCE_API UTwinDataSourceMappingAsset : public UDataAsset
{
	GENERATED_BODY()

public:
/** 显式地址 Mapping；匹配成功时优先于自动推断规则。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	TArray<FTwinDataSourceMapping> Mappings;
};

/**
 * 外部数据接入与 Mapping的项目设置。
 * 运行时只读取配置，不在这里保存连接产生的临时状态。
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Titan Twin Data Sources"))
class TITANTWINDATASOURCE_API UTwinDataSourceSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UTwinDataSourceSettings();

	virtual FName GetCategoryName() const override
	{
		return TEXT("Game");
	}

	FName ResolveUnit(
		FName PropertyName,
		FName IncomingUnit,
		bool bOverrideUnit,
		FName OverrideUnit) const;

	bool TryBuildAutomaticMapping(
		const FTwinDataPoint& Point,
		FTwinDataSourceMapping& OutMapping, bool bAllowFallback = true) const;

/** UTwinDataSourceSettings 使用的 Mapping Assets 集合；运行时会复制配置，避免长期持有编辑器容器引用。 */
	UPROPERTY(Config, EditAnywhere, Category = "Mapping")
	TArray<TSoftObjectPtr<UTwinDataSourceMappingAsset>> MappingAssets;

/** 显式 Mapping 未命中时是否尝试自动解析地址。 */
	UPROPERTY(Config, EditAnywhere, Category = "Mapping|Automatic")
	bool bEnableAutomaticMapping = false;
    /** Evaluate explicit address templates at runtime; no fallback guessing, no asset writes, no persistent learning. */
    UPROPERTY(Config, EditAnywhere, Category="Mapping|Runtime Rules")
    bool bEnableRuntimeRuleMapping = false;
    UPROPERTY(Config, EditAnywhere, Category="Mapping|Runtime Rules", meta=(ClampMin="1",ClampMax="65536"))
    int32 MaxRuntimeRuleAddresses = 2048;

	UPROPERTY(Config, EditAnywhere, Category = "Mapping|Automatic", meta = (ClampMin = "1", ClampMax = "65536"))
	int32 MaxAutomaticMappings = 2048;
	UPROPERTY(Config, EditAnywhere, Category = "Mapping|Automatic", meta = (ClampMin = "0.1", ClampMax = "60.0"))
	float AutomaticMappingSaveIntervalSeconds = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Mapping|Automatic", meta = (EditCondition = "bEnableAutomaticMapping"))
	TSoftObjectPtr<UTwinDataSourceMappingAsset> AutomaticMappingAsset;

	/**
	 * 可配置规则从上往下匹配，最后才会走旧的兜底规则。
	 * 已经注册的 Mapping 永远比自动规则优先。
	 */
	UPROPERTY(
		Config,
		EditAnywhere,
		Category = "Mapping|Automatic",
		meta = (EditCondition = "bEnableAutomaticMapping || bEnableRuntimeRuleMapping", TitleProperty = "AddressTemplate"))
	TArray<FTwinAutomaticMappingRule> AutomaticMappingRules;

	/** 保留原来的规则：取最后两个用斜杠或点分开的字段。 */
	UPROPERTY(
		Config,
		EditAnywhere,
		Category = "Mapping|Automatic",
		meta = (EditCondition = "bEnableAutomaticMapping", DisplayName = "Use Last Two Segments Fallback"))
	bool bUseLastTwoSegmentsFallback = true;

/** UTwinDataSourceSettings 使用的 Property Unit Rules 集合；运行时会复制配置，避免长期持有编辑器容器引用。 */
	UPROPERTY(Config, EditAnywhere, Category = "Units")
	TMap<FName, FName> PropertyUnitRules;

/** Max Pending Points 上限，用于阻止队列或内存无界增长。 */
	UPROPERTY(Config, EditAnywhere, Category = "Ingestion", meta = (ClampMin = "128", ClampMax = "1048576"))
	int32 MaxPendingPoints = 8192;

	/** Total retained sample bytes, including strings; applies before queue copies. */
	UPROPERTY(Config, EditAnywhere, Category = "Ingestion", meta = (ClampMin = "65536", ClampMax = "268435456"))
	int64 MaxPendingBytes = 32 * 1024 * 1024;

/** Max Points Per Pump 上限，用于阻止队列或内存无界增长。 */
	UPROPERTY(Config, EditAnywhere, Category = "Ingestion", meta = (ClampMin = "1", ClampMax = "65536"))
	int32 MaxPointsPerPump = 512;

/** 每次 Pump 在游戏线程最多使用的时间；设为 0 表示只受条数上限控制。 */
	UPROPERTY(
		Config,
		EditAnywhere,
		Category = "Ingestion",
		meta = (ClampMin = "0.0", ClampMax = "50.0", Units = "ms"))
	float PumpTimeBudgetMilliseconds = 2.0f;

	/**
	 * 数据点时间戳比本机 UTC 快 5 秒以上时的处理方式。
	 * 开启：改用接收时间并照常写入（计入 ClampedFutureTimestamps 指标并限频告警）。
	 * 关闭：整条数据点被拒绝，只计入 InvalidPoints。
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Ingestion")
	bool bClampFutureTimestamps = true;

/** Pump Interval 时间，单位为秒。 */
	UPROPERTY(Config, EditAnywhere, Category = "Ingestion", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float PumpIntervalSeconds = 1.0f / 60.0f;

/** 实体尚未注册时，是否暂存该实体属性的最新样本。 */
	UPROPERTY(Config, EditAnywhere, Category = "Missing Entity Buffer")
	bool bBufferMissingEntitySamples = true;

/** 最多暂存多少个不同的实体属性；同一实体属性只保留最新样本。 */
	UPROPERTY(
		Config,
		EditAnywhere,
		Category = "Missing Entity Buffer",
		meta = (EditCondition = "bBufferMissingEntitySamples", ClampMin = "1", ClampMax = "65536"))
	int32 MaxPendingEntitySamples = 4096;
	/** Byte budget for retained missing-entity samples, separate from the ingress queue. */
	UPROPERTY(Config, EditAnywhere, Category = "Missing Entity Buffer", meta = (ClampMin="65536", ClampMax="268435456"))
	int64 MaxPendingEntityBytes = 32 * 1024 * 1024;

/** 暂存样本的有效时间，超时后不再向迟到的实体回放。 */
	UPROPERTY(
		Config,
		EditAnywhere,
		Category = "Missing Entity Buffer",
		meta = (EditCondition = "bBufferMissingEntitySamples", ClampMin = "0.1", ClampMax = "3600.0", Units = "s"))
	float PendingEntitySampleTtlSeconds = 30.0f;

	bool Validate(FString& OutError) const
	{
		const bool bValid = MaxPendingPoints >= 1 && MaxPendingPoints <= 1048576
			&& MaxPendingBytes >= 65536 && MaxPendingBytes <= 268435456
			&& MaxPointsPerPump >= 1 && MaxPointsPerPump <= 65536
			&& FMath::IsFinite(PumpTimeBudgetMilliseconds) && PumpTimeBudgetMilliseconds >= 0.0f && PumpTimeBudgetMilliseconds <= 50.0f
			&& FMath::IsFinite(PumpIntervalSeconds) && PumpIntervalSeconds >= 0.001f && PumpIntervalSeconds <= 1.0f
			&& MaxPendingEntityBytes >= 65536 && MaxPendingEntityBytes <= 268435456
			&& MaxPendingEntitySamples >= 1 && MaxPendingEntitySamples <= 65536
			&& FMath::IsFinite(PendingEntitySampleTtlSeconds) && PendingEntitySampleTtlSeconds >= 0.1f && PendingEntitySampleTtlSeconds <= 3600.0f
			&& MaxAutomaticMappings >= 1 && MaxAutomaticMappings <= 65536
			&& FMath::IsFinite(AutomaticMappingSaveIntervalSeconds) && AutomaticMappingSaveIntervalSeconds >= 0.1f && AutomaticMappingSaveIntervalSeconds <= 60.0f;
		OutError = bValid ? FString() : TEXT("Data-source queue, pump or automatic mapping limits are invalid.");
		return bValid;
	}
};
