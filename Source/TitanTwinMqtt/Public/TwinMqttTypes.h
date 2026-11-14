// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "TwinMqttTypes.generated.h"

/** MQTT 消息确认级别；级别越高，确认开销和重复投递可能性也不同。 */
UENUM(BlueprintType)
enum class ETwinMqttQualityOfService : uint8
{
	AtMostOnce UMETA(DisplayName = "QoS 0 - At Most Once"),
	AtLeastOnce UMETA(DisplayName = "QoS 1 - At Least Once"),
	ExactlyOnce UMETA(DisplayName = "QoS 2 - Exactly Once")
};

/** MQTT Payload 按单值还是 JSON 解析。 */
UENUM(BlueprintType)
enum class ETwinMqttPayloadFormat : uint8
{
	Scalar,
	Json,
    JsonBatch
};

/** MQTT 单值载荷转换成 FTwinValue 时使用的类型。 */
UENUM(BlueprintType)
enum class ETwinMqttValueType : uint8
{
	Auto,
	Boolean,
	Integer,
	Number,
	Text
};

USTRUCT(BlueprintType)
struct TITANTWINMQTT_API FTwinMqttJsonField
{
    GENERATED_BODY()
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="JSON")
    FString ValuePath;
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="JSON")
    FString PropertyName;
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="JSON")
    ETwinMqttValueType ValueType = ETwinMqttValueType::Auto;
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="JSON")
    FName Unit;
};

/**
 * 一个 MQTT 订阅及其 Payload 解析配置。
 * Topic 负责定位消息，JSON 字段只负责从载荷中取值。
 */
USTRUCT(BlueprintType)
struct TITANTWINMQTT_API FTwinMqttSubscriptionDefinition
{
	GENERATED_BODY()

/** 是否订阅这一项，关闭不会删掉 Topic 配置。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Subscription")
	bool bEnabled = true;

/** MQTT 订阅过滤器，可使用 + 和 # 通配符。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Subscription")
	FString TopicFilter;

/** MQTT QoS 等级，决定消息确认和可能重复投递的方式。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Subscription")
	ETwinMqttQualityOfService QualityOfService = ETwinMqttQualityOfService::AtLeastOnce;

/** 载荷解析格式；必须与发布端实际发送的数据一致。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Payload")
	ETwinMqttPayloadFormat PayloadFormat = ETwinMqttPayloadFormat::Scalar;

/** Scalar 载荷转换成 FTwinValue 时使用的目标类型。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Payload")
	ETwinMqttValueType ValueType = ETwinMqttValueType::Auto;

	UPROPERTY(
		Config,
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Payload|JSON",
		meta = (EditCondition = "PayloadFormat == ETwinMqttPayloadFormat::Json", EditConditionHides))
	FString JsonValueField = TEXT("value");

	UPROPERTY(
		Config,
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Payload|JSON",
		meta = (EditCondition = "PayloadFormat == ETwinMqttPayloadFormat::Json", EditConditionHides))
	FString JsonTimestampField = TEXT("timestamp");

	UPROPERTY(
		Config,
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Payload|JSON",
		meta = (EditCondition = "PayloadFormat == ETwinMqttPayloadFormat::Json", EditConditionHides))
	FString JsonQualityField = TEXT("quality");

	UPROPERTY(
		Config,
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Payload|JSON",
		meta = (EditCondition = "PayloadFormat == ETwinMqttPayloadFormat::Json", EditConditionHides))
	FString JsonUnitField = TEXT("unit");

/** 载荷没有单位信息时写入样本的默认单位。 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Payload")
	FName DefaultUnit;
    // Empty means one device object at the root; otherwise an array at this object path.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Payload|Batch")
    FString JsonItemsPath;
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Payload|Batch")
    FString JsonEntityIdField = TEXT("deviceId");
    // Optional root identifier, e.g. line_id. Use {group} in the address template.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Payload|Batch")
    FString JsonGroupIdField;
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Payload|Batch")
    FString JsonAddressTemplate = TEXT("factory/{entity}/{property}");
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Payload|Batch")
    TArray<FTwinMqttJsonField> JsonFields;
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Payload|Batch",meta=(ClampMin="1",ClampMax="4096"))
    int32 MaxBatchPoints = 256;
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Payload|Batch",meta=(ClampMin="1024",ClampMax="16777216"))
    int32 MaxBatchBytes = 1024*1024;

	bool IsValid(FString* OutReason = nullptr) const;
};

/**
 * MQTT 传输的累计运行指标快照。
 * 字段用于诊断和界面展示，不参与业务状态判断。
 */
USTRUCT(BlueprintType)
struct TITANTWINMQTT_API FTwinMqttMetrics
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 ConnectAttempts = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 SuccessfulConnections = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 Reconnects = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 ReceivedMessages = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 ParsedPoints = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 ParseFailures = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 OversizedMessages = 0;

/** Queue Rejected Messages 的运行参数，修改后在下一次启动或调度时生效。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 QueueRejectedMessages = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 DroppedNativeMessages = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 ProtocolErrors = 0;

/** Received 的累计字节数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 ReceivedBytes = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 PublishedMessages = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Twin|MQTT")
	int64 PublishFailures = 0;
};
