// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "TitanTwinTypes.h"
#include "TwinDataSourceTypes.generated.h"

namespace TwinDataSourceLimits
{
	// External clock errors must not advance an entity's ordering watermark by hours.
	inline constexpr double MaximumFutureClockSkewSeconds = 5.0;
}

/** 一个外部数据源当前的运行状态。 */
UENUM(BlueprintType)
enum class ETwinDataSourceState : uint8
{
	Stopped,
	Starting,
	Running,
	Degraded,
	Reconnecting,
	Failed
};

/**
 * 协议解析完成后交给数据源层的数据点。
 * 这里只保留协议地址和样本，实体定位仍由 Mapping 完成。
 */
USTRUCT(BlueprintType)
struct TITANTWINDATASOURCE_API FTwinDataPoint
{
	GENERATED_BODY()

/** 数据源的稳定标识，必须与连接设置和 Mapping 中的名称一致。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Data Source")
	FName SourceId;

/** 协议侧原始地址，例如 MQTT Topic、OPC UA NodeId 或 HTTP 字段路径。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Data Source")
	FString Address;

/** 当前传输或保存的属性值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Data Source")
	FTwinValue Value;

/** 数据产生时间，统一使用 UTC，避免跨节点时区不一致。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Data Source")
	FDateTime TimestampUtc;

/** 外部数据质量；Bad 和 Uncertain 是否采用由上层规则决定。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Data Source")
	ETwinDataQuality Quality = ETwinDataQuality::Good;

/** 属性单位，只描述数据含义，不在这里自动换算数值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Data Source")
	FName Unit;

	bool IsValid(FString* OutReason = nullptr) const;
};

/**
 * 外部数据接入与 Mapping的一条可配置规则。
 * Enabled 关闭时保留配置，但运行时不会应用。
 */
USTRUCT(BlueprintType)
struct TITANTWINDATASOURCE_API FTwinDataSourceMapping
{
	GENERATED_BODY()

	// 填 None 就匹配所有发布这个地址的数据源。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source")
	FName SourceId;

/** 协议侧原始地址，例如 MQTT Topic、OPC UA NodeId 或 HTTP 字段路径。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Source")
	FString Address;

/** 实体的稳定标识；Mapping、查询和命令路由都使用这个值。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Target")
	FName EntityId;

/** 实体上的动态属性名，大小写按 FName 规则处理。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Target")
	FName PropertyName;

/** 是否用 Mapping 中的单位覆盖数据源自带单位。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Target")
	bool bOverrideUnit = false;

/** 属性单位，只描述数据含义，不在这里自动换算数值。 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Target",
		meta = (EditCondition = "bOverrideUnit", EditConditionHides))
	FName Unit;

/** 关掉的 Mapping 不参与匹配，内容仍然保留。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Target")
	bool bEnabled = true;

	bool IsValid(FString* OutReason = nullptr) const;
};

/**
 * 数据源最近一次上报的状态。
 * 时间和错误信息用于界面诊断，不参与 Mapping。
 */
USTRUCT(BlueprintType)
struct TITANTWINDATASOURCE_API FTwinDataSourceStatus
{
	GENERATED_BODY()

/** 数据源的稳定标识，必须与连接设置和 Mapping 中的名称一致。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	FName SourceId;

/** 当前生命周期状态，修改时必须经过对应子系统的状态入口。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	ETwinDataSourceState State = ETwinDataSourceState::Stopped;

/** 最近一次运行错误，后续成功操作可以将其清空。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	FString LastError;

/** Last Changed 的 UTC 时间。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	FDateTime LastChangedUtc;
};

/**
 * 外部数据接入与 Mapping的累计运行指标快照。
 * 字段用于诊断和界面展示，不参与业务状态判断。
 */
USTRUCT(BlueprintType)
struct TITANTWINDATASOURCE_API FTwinDataSourceMetrics
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 AcceptedPoints = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 AppliedSamples = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 InvalidPoints = 0;

	/** 时间戳超前过多、已改用接收时间写入的数据点数量。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 ClampedFutureTimestamps = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 UnmappedPoints = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 MissingEntityTargets = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 DroppedByBackpressure = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int32 PendingPoints = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 PendingBytes = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Twin|Data Source") int64 PendingEntityBytes = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Twin|Data Source") int64 PeakPendingEntityBytes = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int32 RegisteredMappings = 0;

	/** 等待目标实体注册的实体属性数量。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int32 PendingEntitySamples = 0;

	/** 实体注册后成功回放的暂存样本数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 ReplayedPendingEntitySamples = 0;

	/** 因超过有效时间而清除的暂存样本数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 ExpiredPendingEntitySamples = 0;

	/** 缓存达到容量上限时被淘汰的暂存样本数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Data Source")
	int64 DroppedPendingEntitySamples = 0;
};
