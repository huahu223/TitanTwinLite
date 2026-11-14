// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/WorldSubsystem.h"
#include "TwinDataSourceTypes.h"
#include "TwinMqttTypes.h"
#include "TwinMqttSubsystem.generated.h"

class FTwinMqttNativeClient;
struct FTwinMqttNativeEvent;
struct FTwinMqttRuntimeState;
struct FTwinMqttPersistentSession;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTwinMqttConnectionChangedSignature, bool, bConnected);

/**
 * 管理 MQTT 客户端生命周期并把订阅数据交给数据源层。
 * 网络线程不接触 UObject，所有运行时事件统一泵回游戏线程。
 */
UCLASS()
class TITANTWINMQTT_API UTwinMqttSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

/** MQTT 连接状态变化后在游戏线程广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|MQTT")
	FTwinMqttConnectionChangedSignature OnConnectionChanged;

/**
 * 按当前设置启动异步连接。
 * 返回值只表示连接流程是否成功创建，最终状态通过事件或查询获得。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|MQTT")
	bool Connect();

/**
 * 主动断开并取消重连计时器。
 * 旧连接迟到的回调会被运行代数丢弃。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|MQTT")
	void Disconnect();

/**
 * 作废当前连接后重新读取设置并连接。
 * 返回 true 只表示新的连接流程已经开始。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|MQTT")
	bool Reconnect();

/**
 * 返回最近确认的连接状态。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|MQTT")
	bool IsConnected() const;

/**
 * 返回是否已有连接尝试尚未结束。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|MQTT")
	bool IsConnecting() const;

/**
 * 返回最近一次运行错误；后续成功操作可能清空该值。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|MQTT")
	FString GetLastError() const;

/**
 * 拿到 MQTT 连接、收发和解析计数的当前值。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|MQTT")
	FTwinMqttMetrics GetMetrics() const;

/**
 * 发布原始 MQTT 载荷。
 * 返回 true 只表示数据已交给客户端，QoS 确认可能稍后到达。
 * @param Topic 要发布或处理的 MQTT Topic。
 * @param Payload 要发布或处理的原始载荷。
 * @param QualityOfService 本次发布使用的 MQTT QoS。
 * @param bRetain 是否让 Broker 保留最后一条消息。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|MQTT")
	bool Publish(
		const FString& Topic,
		const FString& Payload,
		ETwinMqttQualityOfService QualityOfService = ETwinMqttQualityOfService::AtLeastOnce,
		bool bRetain = false);

private:
	bool PumpNativeEvents(float DeltaSeconds);
	void HandleNativeEvent(FTwinMqttNativeEvent&& Event);
	void HandleReconnectTimer();
	void ScheduleReconnect(const FString& Reason);
	void ClearReconnectTimer();
	uint64 InvalidateConnection();
	void ReleaseClient();
	void ReportSourceState(ETwinDataSourceState State, const FString& ErrorMessage = FString()) const;
	bool ValidateSettings(FString& OutError) const;

	TSharedPtr<FTwinMqttNativeClient, ESPMode::ThreadSafe> Client;
	TSharedPtr<FTwinMqttRuntimeState, ESPMode::ThreadSafe> RuntimeState;
	TSharedPtr<FTwinMqttPersistentSession, ESPMode::ThreadSafe> PersistentSession;
	FString PersistentSessionIdentity;

	FTSTicker::FDelegateHandle NativePumpTickerHandle;
	FTimerHandle ReconnectTimerHandle;
	FName ActiveSourceId = TEXT("Mqtt");
	FString LastError;
	int32 ReconnectAttempt = 0;
	uint64 ConnectionGeneration = 0;
	bool bConnected = false;
	bool bConnecting = false;
	bool bManualDisconnect = false;
};
