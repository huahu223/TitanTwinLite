// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "TitanTwinTypes.generated.h"

/** 实体数据连接状态；只表示是否在线，不代表设备正在工作。 */
UENUM(BlueprintType)
enum class ETwinConnectionState : uint8
{
	Unknown,
	Offline,
	Online
};

/** 设备运行状态，由业务数据或外部系统明确写入。 */
UENUM(BlueprintType)
enum class ETwinOperationalState : uint8
{
	Unknown,
	Idle,
	Active
};

/** 设备健康状态；插件不会自动从告警反推这个值。 */
UENUM(BlueprintType)
enum class ETwinHealthState : uint8
{
	Unknown,
	Normal,
	Warning,
	Fault
};

// 这是给早期 TitanTwin 原型项目保留的兼容写法。
UENUM(BlueprintType)
enum class ETwinEntityStatus : uint8
{
	Unknown,
	Offline,
	Idle,
	Active,
	Warning,
	Fault
};

/** 外部采样质量，用于决定坏数据能否进入属性、历史或告警。 */
UENUM(BlueprintType)
enum class ETwinDataQuality : uint8
{
	Unknown,
	Good,
	Uncertain,
	Bad
};

/** FTwinValue 当前实际保存的数据类型。 */
UENUM(BlueprintType)
enum class ETwinValueType : uint8
{
	None,
	Boolean,
	Integer,
	Number,
	Text,
	Vector,
	Rotator
};

/**
 * 插件统一使用的动态值容器。
 * Type 决定当前读取哪个存储字段，未设置值不能参与比较或序列化。
 */
USTRUCT(BlueprintType)
struct TITANTWINRUNTIME_API FTwinValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Twin")
	ETwinValueType Type = ETwinValueType::None;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Twin",
		meta = (EditCondition = "Type == ETwinValueType::Boolean", EditConditionHides))
	bool BooleanValue = false;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Twin",
		meta = (EditCondition = "Type == ETwinValueType::Integer", EditConditionHides))
	int64 IntegerValue = 0;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Twin",
		meta = (EditCondition = "Type == ETwinValueType::Number", EditConditionHides))
	double NumberValue = 0.0;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Twin",
		meta = (EditCondition = "Type == ETwinValueType::Text", EditConditionHides))
	FString TextValue;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Twin",
		meta = (EditCondition = "Type == ETwinValueType::Vector", EditConditionHides))
	FVector VectorValue = FVector::ZeroVector;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadOnly,
		Category = "Twin",
		meta = (EditCondition = "Type == ETwinValueType::Rotator", EditConditionHides))
	FRotator RotatorValue = FRotator::ZeroRotator;

	static FTwinValue MakeBoolean(bool Value);
	static FTwinValue MakeInteger(int64 Value);
	static FTwinValue MakeNumber(double Value);
	static FTwinValue MakeText(const FString& Value);
	static FTwinValue MakeVector(const FVector& Value);
	static FTwinValue MakeRotator(const FRotator& Value);

	bool IsSet() const;
	bool IsNearlyEqual(const FTwinValue& Other, double Tolerance = UE_DOUBLE_SMALL_NUMBER) const;
	bool operator==(const FTwinValue& Other) const;
	bool operator!=(const FTwinValue& Other) const
	{
		return !(*this == Other);
	}
};

/**
 * 一次完整的属性采样。
 * 除数值外还保留时间、质量、单位和来源，外部协议应尽量使用该结构。
 */
USTRUCT(BlueprintType)
struct TITANTWINRUNTIME_API FTwinPropertySample
{
	GENERATED_BODY()

/** 当前传输或保存的属性值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin")
	FTwinValue Value;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin")
	FDateTime Timestamp;

/** 外部数据质量；Bad 和 Uncertain 是否采用由上层规则决定。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin")
	ETwinDataQuality Quality = ETwinDataQuality::Good;

/** 属性单位，只描述数据含义，不在这里自动换算数值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin")
	FName Unit;

/** 产生这条数据的来源标识，用于追踪和过滤。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin")
	FName Source = TEXT("Local");

	bool IsEquivalentForNotification(const FTwinPropertySample& Other) const;
};

/** 给蓝图提供实体、属性和世界注册的无状态辅助入口。 */
UCLASS()
class TITANTWINRUNTIME_API UTwinValueLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
/**
 * 构造布尔类型的 FTwinValue。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Value")
	static FTwinValue MakeTwinBoolean(bool Value);

/**
 * 构造 64 位整数类型的 FTwinValue。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Value")
	static FTwinValue MakeTwinInteger(int64 Value);

/**
 * 构造双精度数值类型的 FTwinValue。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Value")
	static FTwinValue MakeTwinNumber(double Value);

/**
 * 构造文本类型的 FTwinValue。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Value")
	static FTwinValue MakeTwinText(const FString& Value);

/**
 * 构造三维向量类型的 FTwinValue。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Value")
	static FTwinValue MakeTwinVector(FVector Value);

/**
 * 构造旋转类型的 FTwinValue。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Value")
	static FTwinValue MakeTwinRotator(FRotator Value);

/**
 * 为本地值补齐时间、质量、单位和来源，生成完整样本。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Value")
	static FTwinPropertySample MakeTwinSample(
		const FTwinValue& Value,
		FName Unit = NAME_None,
		ETwinDataQuality Quality = ETwinDataQuality::Good,
		FName Source = TEXT("Local"));
};
