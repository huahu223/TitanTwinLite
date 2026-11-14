// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "TwinMqttTypes.h"
#include "TwinMqttSettings.generated.h"

/**
 * MQTT 传输的项目设置。
 * 运行时只读取配置，不在这里保存连接产生的临时状态。
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Titan Twin MQTT"))
class TITANTWINMQTT_API UTwinMqttSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override
	{
		return TEXT("Game");
	}

/** 数据源的稳定标识，必须与连接设置和 Mapping 中的名称一致。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	FName SourceId = TEXT("Mqtt");

/** 服务端主机名或 IP 地址。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	FString Host = TEXT("127.0.0.1");

/** 服务端端口，必须处于有效的 TCP 端口范围。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 Port = 1883;

/** 是否启用 TLS；开启后证书和服务端名称也必须有效。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	bool bUseTls = false;
    /** Optional PEM CA bundle, relative to project directory or absolute; engine trust roots remain enabled. */
    UPROPERTY(Config, EditAnywhere, Category="Connection|TLS", meta=(EditCondition="bUseTls"))
    FString TlsCaFile;

/** 是否在连接时要求 Broker 丢弃旧会话状态。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	bool bCleanSession = true;

/** MQTT 客户端标识；为空时运行时生成不会冲突的值。 */
	UPROPERTY(
		Config,
		EditAnywhere,
		Category = "Connection",
		meta = (ToolTip = "Stable MQTT client identifier. Required when Clean Session is disabled. Leave empty with Clean Session enabled to generate an ephemeral ID."))
	FString ClientId;

/** World 开始运行后是否自动建立连接。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	bool bAutoConnect = false;

/** 意外断线后是否按照退避策略自动重连。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	bool bAutoReconnect = true;

/** MQTT Keep Alive 间隔，单位为秒。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 KeepAliveSeconds = 30;

/** 建立连接的最长等待时间，单位为秒。 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (ClampMin = "0.1", ClampMax = "300.0"))
	float ConnectTimeoutSeconds = 10.0f;

/** 保存用户名的环境变量名，避免把凭据写进项目配置。 */
	UPROPERTY(Config, EditAnywhere, Category = "Security")
	FString UsernameEnvironmentVariable = TEXT("TITAN_MQTT_USERNAME");

/** 保存密码的环境变量名，运行时只读取环境变量内容。 */
	UPROPERTY(Config, EditAnywhere, Category = "Security")
	FString PasswordEnvironmentVariable = TEXT("TITAN_MQTT_PASSWORD");

/** 启动连接后需要建立的 MQTT 订阅集合。 */
	UPROPERTY(Config, EditAnywhere, Category = "Subscriptions")
	TArray<FTwinMqttSubscriptionDefinition> Subscriptions;

/** 首次重连等待时间，单位为秒。 */
	UPROPERTY(Config, EditAnywhere, Category = "Reliability", meta = (ClampMin = "0.1", ClampMax = "300.0"))
	float ReconnectInitialDelaySeconds = 1.0f;

/** 重连退避允许达到的最长等待时间，单位为秒。 */
	UPROPERTY(Config, EditAnywhere, Category = "Reliability", meta = (ClampMin = "1.0", ClampMax = "3600.0"))
	float ReconnectMaxDelaySeconds = 30.0f;

/** 单条网络载荷允许的最大字节数；达到上限后按对应模块的保护策略处理。 */
	UPROPERTY(Config, EditAnywhere, Category = "Safety", meta = (ClampMin = "1", ClampMax = "16777216"))
	int32 MaxPayloadBytes = 1048576;

/** Max Pending Native Messages 上限，用于阻止队列或内存无界增长。 */
	UPROPERTY(Config, EditAnywhere, Category = "Safety", meta = (ClampMin = "16", ClampMax = "65536"))
	int32 MaxPendingNativeMessages = 4096;

/** Max Pending Publishes 上限，用于阻止队列或内存无界增长。 */
	UPROPERTY(Config, EditAnywhere, Category = "Safety", meta = (ClampMin = "16", ClampMax = "65536"))
	int32 MaxPendingPublishes = 4096;
	/** Shared budget for native message events, pending publishes and persisted QoS state. */
	UPROPERTY(Config, EditAnywhere, Category = "Safety", meta = (ClampMin = "65536", ClampMax = "268435456"))
	int64 MaxRetainedNativeBytes = 64 * 1024 * 1024;

/** Max Native Messages Per Pump 上限，用于阻止队列或内存无界增长。 */
	UPROPERTY(Config, EditAnywhere, Category = "Safety", meta = (ClampMin = "1", ClampMax = "8192"))
	int32 MaxNativeMessagesPerPump = 512;

/** Max Pending Qos 2 Messages 上限，用于阻止队列或内存无界增长。 */
	UPROPERTY(Config, EditAnywhere, Category = "Safety", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 MaxPendingQos2Messages = 1024;

/** Qos 2 Message Timeout 时间，单位为秒。 */
	UPROPERTY(Config, EditAnywhere, Category = "Safety", meta = (ClampMin = "1.0", ClampMax = "3600.0"))
	float Qos2MessageTimeoutSeconds = 60.0f;
};
