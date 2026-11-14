// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "TitanTwinTypes.h"
#include "TwinEntityComponent.generated.h"

struct FTwinEntitySampleQueue;

class UTwinWorldSubsystem;
class UTwinEntityComponent;

DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FTwinConnectionStateChangedNativeSignature,
	UTwinEntityComponent*,
	ETwinConnectionState,
	ETwinConnectionState);

DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FTwinOperationalStateChangedNativeSignature,
	UTwinEntityComponent*,
	ETwinOperationalState,
	ETwinOperationalState);

DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FTwinHealthStateChangedNativeSignature,
	UTwinEntityComponent*,
	ETwinHealthState,
	ETwinHealthState);

DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FTwinPropertySampleChangedNativeSignature,
	UTwinEntityComponent*,
	FName,
	const FTwinPropertySample&);

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FTwinPropertyRemovedNativeSignature,
	UTwinEntityComponent*,
	FName);

// 对外修改数据的接口只能在游戏线程调用。属性样本就是一条遥测数据。
// 即使值没变，只要时间戳是新的并且有效，也会广播一次。

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinStatusChangedSignature,
	ETwinEntityStatus,
	PreviousStatus,
	ETwinEntityStatus,
	NewStatus);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinConnectionStateChangedSignature,
	ETwinConnectionState,
	PreviousState,
	ETwinConnectionState,
	NewState);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinOperationalStateChangedSignature,
	ETwinOperationalState,
	PreviousState,
	ETwinOperationalState,
	NewState);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinHealthStateChangedSignature,
	ETwinHealthState,
	PreviousState,
	ETwinHealthState,
	NewState);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinPropertyChangedSignature,
	FName,
	PropertyName,
	const FTwinValue&,
	NewValue);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinPropertySampleChangedSignature,
	FName,
	PropertyName,
	const FTwinPropertySample&,
	NewSample);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FTwinPropertyRemovedSignature,
	FName,
	PropertyName);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinEntityIdChangedSignature,
	FName,
	PreviousEntityId,
	FName,
	NewEntityId);

DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FTwinEntityIdChangedNativeSignature,
	UTwinEntityComponent*,
	FName,
	FName);

/**
 * 挂在 Actor 上的数字孪生实体组件。
 * 组件在 BeginPlay 后注册到当前 World，所有对外修改接口都要求游戏线程。
 */
UCLASS(ClassGroup = "Digital Twin", meta = (BlueprintSpawnableComponent))
class TITANTWINRUNTIME_API UTwinEntityComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTwinEntityComponent();

/** 实体类型标签，用于批量查询和界面分类。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Twin|Identity")
	FName EntityType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Twin|Identity")
	FGameplayTagContainer EntityTags;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Twin|Identity", meta = (ClampMin = "1"))
	int32 SchemaVersion = 1;

/** 连接状态实际变化后在游戏线程广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinConnectionStateChangedSignature OnConnectionStateChanged;

/** 实体运行状态变化后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinOperationalStateChangedSignature OnOperationalStateChanged;

/** 实体健康状态变化后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinHealthStateChangedSignature OnHealthStateChanged;

	// 这个是为了兼容旧项目保留的，新代码直接用上面的三个状态事件。
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinStatusChangedSignature OnStatusChanged;

/** 动态属性的值发生变化后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinPropertyChangedSignature OnPropertyChanged;

/** 样本值或采样信息发生变化后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinPropertySampleChangedSignature OnPropertySampleChanged;

/** 动态属性被删除后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinPropertyRemovedSignature OnPropertyRemoved;

/** 实体完成重新注册并更换 ID 后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Events")
	FTwinEntityIdChangedSignature OnEntityIdChanged;

	FTwinConnectionStateChangedNativeSignature OnConnectionStateChangedNative;
	FTwinOperationalStateChangedNativeSignature OnOperationalStateChangedNative;
	FTwinHealthStateChangedNativeSignature OnHealthStateChangedNative;
	FTwinPropertySampleChangedNativeSignature OnPropertySampleChangedNative;
	FTwinPropertyRemovedNativeSignature OnPropertyRemovedNative;
	FTwinEntityIdChangedNativeSignature OnEntityIdChangedNative;

/**
 * 返回当前稳定实体标识。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Identity")
	FName GetEntityId() const;

/**
 * 返回当前实例 GUID；组件重新创建后该值会变化。
 * @return 创建成功时返回有效 ID，失败时返回无效 ID。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Identity")
	FGuid GetEntityGuid() const;

/**
 * 修改实体标识并同步更新世界注册表。
 * 冲突或无效名称会返回 false。
 * @param NewEntityId 目标 New Entity 标识。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Identity")
	bool SetEntityId(FName NewEntityId);

/** 修改实体类型，并让按类型查询立即看到新值。 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Identity")
	void SetEntityType(FName NewEntityType);

/**
 * 返回组件当前是否已经进入所属 World 的注册表。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Registry")
	bool IsRegistered() const;

/**
 * 更新连接状态；值变化时在游戏线程广播。
 * @param NewState 准备写入的新状态。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|State")
	void SetConnectionState(ETwinConnectionState NewState);

/**
 * 返回游戏线程缓存的连接状态。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|State")
	ETwinConnectionState GetConnectionState() const;

/**
 * 更新运行状态；插件不会根据属性值自动推断该状态。
 * @param NewState 准备写入的新状态。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|State")
	void SetOperationalState(ETwinOperationalState NewState);

/**
 * 返回外部系统或业务逻辑最后一次写入的运行状态。
 * 插件不会根据普通属性值自动推断该状态。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|State")
	ETwinOperationalState GetOperationalState() const;

/**
 * 更新健康状态；告警系统不会自动覆盖该值。
 * @param NewState 准备写入的新状态。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|State")
	void SetHealthState(ETwinHealthState NewState);

/**
 * 返回外部系统或业务逻辑最后一次写入的健康状态。
 * 告警状态不会自动覆盖这里的值。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|State")
	ETwinHealthState GetHealthState() const;

/** 旧版统一状态入口，仅用于兼容已有蓝图。 */
	UFUNCTION(
		BlueprintCallable,
		Category = "Twin|State",
		meta = (DeprecatedFunction, DeprecationMessage = "Use connection, operational and health states instead."))
	void SetStatus(ETwinEntityStatus NewStatus);

/**
 * 返回旧版兼容状态，新代码应分别读取三种状态。
 */
	UFUNCTION(
		BlueprintPure,
		Category = "Twin|State",
		meta = (DeprecatedFunction, DeprecationMessage = "Use connection, operational and health states instead."))
	ETwinEntityStatus GetStatus() const;

/**
 * 写入一个值并补齐本地样本信息。
 * 连续外部数据应优先调用 SetPropertySample 保留原始时间和质量。
 * @param PropertyName 目标动态属性名。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Properties")
	void SetProperty(FName PropertyName, const FTwinValue& NewValue);

	// 样本有效并且保存成功就返回 true。
	// 只有整个样本都没变，连时间戳也一样时，才可能不触发事件。
	// 重入通知按写入顺序串行分发；达到每次分发预算时，剩余通知延后到后续帧。
	// 通知队列满时在修改属性前返回 false。
	UFUNCTION(BlueprintCallable, Category = "Twin|Properties")
	bool SetPropertySample(FName PropertyName, const FTwinPropertySample& NewSample);

	// MQTT、WebSocket 或其他工作线程回调统一从这里进入，线程安全。
	void EnqueuePropertySample(FName PropertyName, const FTwinPropertySample& NewSample);

	/** Return false when input is invalid, the component has stopped, or its byte/count budget is full. */
	bool TryEnqueuePropertySample(FName PropertyName, const FTwinPropertySample& NewSample);
	UFUNCTION(BlueprintPure, Category = "Twin|Properties|Diagnostics")
	int32 GetPendingPropertySampleCount() const;
	UFUNCTION(BlueprintPure, Category = "Twin|Properties|Diagnostics")
	int64 GetDroppedPropertySampleCount() const;

/**
 * 读取属性值；属性不存在时不修改输出参数并返回 false。
 * @param PropertyName 目标动态属性名。
 * @param OutValue 读取成功后接收属性值。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Properties")
	bool GetProperty(FName PropertyName, FTwinValue& OutValue) const;

/**
 * 读取完整样本，包括时间、质量、单位和来源。
 * @param PropertyName 目标动态属性名。
 * @param OutSample 读取成功后接收完整样本。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Properties")
	bool GetPropertySample(FName PropertyName, FTwinPropertySample& OutSample) const;
	/** Real-time timestamp watermark while a historical view is active. Game thread only. */
	bool GetLivePropertySample(FName PropertyName, FTwinPropertySample& OutSample) const;

/**
 * 返回当前实体动态属性名的快照。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Properties")
	TArray<FName> GetPropertyNames() const;

/**
 * 删除指定动态属性；不存在时返回 false。
 * @param PropertyName 目标动态属性名。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Properties")
	bool RemoveProperty(FName PropertyName);

/** 清空动态属性并逐项发出删除通知。 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Properties")
	void ClearProperties();

protected:
	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditImport() override;
#endif

private:
	friend class UTwinWorldSubsystem;
	friend class UTwinHistorySubsystem;
	// Separate historian feed: live samples remain recordable while the visible model replays.
	FTwinPropertySampleChangedNativeSignature OnReplayLiveSample;
	FTwinPropertyRemovedNativeSignature OnReplayLiveRemoval;
	FTwinConnectionStateChangedNativeSignature OnReplayLiveConnection;
	FTwinOperationalStateChangedNativeSignature OnReplayLiveOperational;
	FTwinHealthStateChangedNativeSignature OnReplayLiveHealth;
	void BeginReplayView();
	void FinishReplayView();
	bool ConsumeReplayMutation() { const bool Result=bNextReplayMutation; bNextReplayMutation=false; return Result; }
	bool bReplayViewActive = false;
	bool bNextReplayMutation = false;
	TMap<FName, FTwinPropertySample> LivePropertiesDuringReplay;
	ETwinConnectionState LiveConnectionState = ETwinConnectionState::Unknown;
	ETwinOperationalState LiveOperationalState = ETwinOperationalState::Unknown;
	ETwinHealthState LiveHealthState = ETwinHealthState::Unknown;
	struct FPropertyNotification
	{
		FName Name;
		FTwinPropertySample Sample;
		TArray<FName> RemovedNames;
		bool bRemoved = false;
		int64 Bytes = 0;
	};
	bool QueuePropertyNotification(FPropertyNotification&& Notification);
	void DispatchPropertyNotifications();
	void SchedulePropertyNotifications();
	TArray<FPropertyNotification> PropertyNotifications;
	int64 PropertyNotificationBytes = 0;
	uint64 PropertyNotificationGeneration = 0;
	bool bDispatchingPropertyNotifications = false;
	bool bPropertyNotificationsScheduled = false;
	bool bEndingPlay = false;

	void EnsureIdentity();
	void ScheduleSampleDrain(uint64 Generation);
	bool DrainPendingSamples(uint64 Generation);
	TSharedPtr<FTwinEntitySampleQueue, ESPMode::ThreadSafe> PendingSampleQueue;
	void BroadcastLegacyStatusIfChanged(ETwinEntityStatus PreviousStatus);

/** 实体的稳定标识；Mapping、查询和命令路由都使用这个值。 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Twin|Identity", meta = (AllowPrivateAccess = "true"))
	FName EntityId;

/** 本次实体实例的全局唯一标识，用于区分重名或重新创建的对象。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Twin|Identity", meta = (AllowPrivateAccess = "true"))
	FGuid EntityGuid;

/** 最近一次确认的 Connection State。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|State", meta = (AllowPrivateAccess = "true"))
	ETwinConnectionState ConnectionState = ETwinConnectionState::Unknown;

/** 最近一次确认的 Operational State。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|State", meta = (AllowPrivateAccess = "true"))
	ETwinOperationalState OperationalState = ETwinOperationalState::Unknown;

/** 最近一次确认的 Health State。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|State", meta = (AllowPrivateAccess = "true"))
	ETwinHealthState HealthState = ETwinHealthState::Unknown;

/** UTwinEntityComponent 使用的 Properties 集合；运行时会复制配置，避免长期持有编辑器容器引用。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|Properties", meta = (AllowPrivateAccess = "true"))
	TMap<FName, FTwinPropertySample> Properties;

/** Registered Entity 的稳定标识，用于跨帧或跨协议关联。 */
	UPROPERTY(Transient)
	FName RegisteredEntityId;

/** 组件是否已经进入当前 World 的实体注册表。 */
	UPROPERTY(Transient)
	bool bRegistered = false;

/** 当前实体 ID 是否由组件自动生成。 */
	UPROPERTY()
	bool bEntityIdWasGenerated = false;
};
