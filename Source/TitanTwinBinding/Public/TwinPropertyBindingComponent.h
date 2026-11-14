// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "TitanTwinTypes.h"
#include "TwinPropertyBindingComponent.generated.h"

class UTwinEntityComponent;

UENUM(BlueprintType)
enum class ETwinBindingSampleStatus : uint8 { Missing, Ready, BadQuality, Stale };

/** 属性绑定查找目标控件或对象的范围。 */
UENUM(BlueprintType)
enum class ETwinBindingTargetScope : uint8
{
	OwnerActor UMETA(DisplayName = "Owner Actor"),
	ActorComponent UMETA(DisplayName = "Actor Component"),
	WidgetFromComponent UMETA(DisplayName = "User Widget From Widget Component"),
	AnimInstanceFromComponent UMETA(DisplayName = "Anim Instance From Skeletal Mesh")
};

/**
 * 属性与界面绑定的一条可配置规则。
 * Enabled 关闭时保留配置，但运行时不会应用。
 */
USTRUCT(BlueprintType)
struct TITANTWINBINDING_API FTwinPropertyBinding
{
	GENERATED_BODY()

/** 关掉后保留配置，但不再往目标对象写值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding")
	bool bEnabled = true;

    // Rejected samples leave the last successfully applied value intact.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Twin|Binding|Quality")
    bool bAcceptUncertainQuality = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Twin|Binding|Quality")
    bool bAcceptBadOrUnknownQuality = false;
    // 0 disables expiry, suitable for static properties and command targets.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Twin|Binding|Quality", meta=(ClampMin="0.0", Units="s"))
    float MaximumSampleAgeSeconds = 0.0f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Twin|Binding|Status")
    ETwinBindingSampleStatus SampleStatus = ETwinBindingSampleStatus::Missing;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Twin|Binding|Status")
    FString SampleStatusReason;

	// 这里填要读取的 TwinEntity 动态属性，例如 temperature。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding")
	FName SourcePropertyName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding")
	ETwinBindingTargetScope TargetScope = ETwinBindingTargetScope::OwnerActor;

	// 目标是 OwnerActor 时不用这个。目标是界面就选 Widget Component；
	// 目标是动画就选 Skeletal Mesh Component。
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Twin|Binding",
		meta = (UseComponentPicker))
	FComponentReference TargetComponent;

	// 这是给旧版按名称选择器保存的绑定做兼容。
	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Use TargetComponent instead."))
	FName TargetComponentName;

	// 最终目标对象里能被反射找到的蓝图或 C++ 变量。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding")
	FName TargetVariableName;

	// 只有把数值写进 String、Name 或 Text 时才会用到。
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Twin|Binding|Text",
		meta = (ClampMin = "0", ClampMax = "6"))
	int32 MinimumFractionalDigits = 0;

/** Maximum Fractional Digits 上限，用于阻止队列或内存无界增长。 */
	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category = "Twin|Binding|Text",
		meta = (ClampMin = "0", ClampMax = "6"))
	int32 MaximumFractionalDigits = 2;

/** 整数显示时是否使用本地化千位分组。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding|Text")
	bool bUseGrouping = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding|Text")
	FString TextPrefix;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding|Text")
	FString TextSuffix;

/** Applied 的累计数量。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Twin|Binding|Status")
	int64 AppliedCount = 0;

/** 最近一次运行错误，后续成功操作可以将其清空。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "Twin|Binding|Status")
	FString LastError;

	/** Internal identity used to safely reconcile snapshots after user callbacks. */
	UPROPERTY(Transient)
	FGuid RuntimeBindingId;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FTwinVariableBindingAppliedSignature,
	FName,
	SourcePropertyName,
	UObject*,
	TargetObject,
	FName,
	TargetVariableName);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FTwinVariableBindingFailedSignature,
	FName,
	SourcePropertyName,
	const FString&,
	ErrorMessage);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FTwinBindingSampleStatusSignature, FName, PropertyName, ETwinBindingSampleStatus, Status, const FString&, Reason);

/**
 * 把属性与界面绑定能力挂到 Actor 上的运行时组件。
 * BeginPlay 前对象和实体标识可能仍会被编辑器重建。
 */
UCLASS(ClassGroup = "Digital Twin", meta = (BlueprintSpawnableComponent))
class TITANTWINBINDING_API UTwinPropertyBindingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTwinPropertyBindingComponent();

	// 在蓝图类里配置这些绑定，同一个类生成的所有 Actor 都会继承。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding")
	TArray<FTwinPropertyBinding> Bindings;
    UPROPERTY(BlueprintAssignable, Category="Twin|Binding")
    FTwinBindingSampleStatusSignature OnSampleStatusChanged;
    UFUNCTION(BlueprintCallable, Category="Twin|Binding")
    void RefreshSampleStatuses();
    static ETwinBindingSampleStatus EvaluateSample(const FTwinPropertyBinding& Binding,
        const FTwinPropertySample& Sample, const FDateTime& Now, bool bReplay, FString& Reason);
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;

/** BeginPlay 时是否立即读取现有属性，避免必须等下一条数据。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Twin|Binding")
	bool bApplyExistingValuesOnBeginPlay = true;

/** 属性值成功写入目标对象后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Binding")
	FTwinVariableBindingAppliedSignature OnBindingApplied;

/** 绑定目标无效或类型转换失败时广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Binding")
	FTwinVariableBindingFailedSignature OnBindingFailed;

/**
 * 重新解析实体和目标对象，并重建事件绑定。
 * 旧 Delegate 会先解绑，避免重复回调。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Binding")
	bool Rebind();

/**
 * 立即读取当前实体属性，避免界面必须等待下一次变化事件。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Binding")
	int32 ApplyExistingValues();

/**
 * 检查反射目标、属性类型和转换规则，不修改目标对象。
 */
	UFUNCTION(BlueprintCallable, Category = "Twin|Binding")
	bool ValidateBindings();

/**
 * 返回当前弱引用指向的实体；实体销毁后可能为空。
 * @return 找不到目标或对象已经销毁时返回空。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Binding")
	UTwinEntityComponent* GetBoundEntity() const { return BoundEntity.Get(); }

/** Shared validated assignment path used by property and alarm responses. Game thread only. */
    static bool WriteTargetVariable(UObject* TargetObject, FName TargetVariableName,
        const FTwinValue& Value, const FTwinPropertyBinding& Binding,
        FString& OutError, bool bValidateOnly = false);
    static UObject* ResolveBindingTarget(AActor* Owner, const FTwinPropertyBinding& Binding);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	TArray<FTwinPropertyBinding> SnapshotBindings(FName SourceFilter = NAME_None);
	void UpdateBindingStatus(const FGuid& Id, bool bApplied, const FString& Error);
	void Unbind();
	void HandlePropertySampleChanged(
		UTwinEntityComponent* Entity,
		FName PropertyName,
		const FTwinPropertySample& Sample);
    bool SetSampleStatus(const FTwinPropertyBinding& Binding, ETwinBindingSampleStatus Status, const FString& Reason);
    bool ApplySample(const FTwinPropertyBinding& Binding);
	bool ApplyBinding(FTwinPropertyBinding& Binding, const FTwinValue& Value);
	UObject* ResolveTargetObject(const FTwinPropertyBinding& Binding) const;

	void ReportFailure(FTwinPropertyBinding& Binding, const FString& Error);

	TWeakObjectPtr<UTwinEntityComponent> BoundEntity;
};
