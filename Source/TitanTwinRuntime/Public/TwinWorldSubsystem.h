// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"
#include "TwinWorldSubsystem.generated.h"

class UTwinEntityComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FTwinEntityRegistrySignature,
	UTwinEntityComponent*,
	Entity);

DECLARE_MULTICAST_DELEGATE_OneParam(
	FTwinEntityRegistryNativeSignature,
	UTwinEntityComponent*);

/**
 * 这是一个 UWorld 对应的实体注册表，只在游戏线程使用。
 * 事件监听函数可能在广播过程中重新操作注册表。
 * 所以广播前后不能一直保存容器元素的引用。
 */
UCLASS()
class TITANTWINRUNTIME_API UTwinWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;

/** 实体成功加入当前 World 注册表后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Registry")
	FTwinEntityRegistrySignature OnEntityRegistered;

/** 实体从当前 World 注册表移除后广播。 */
	UPROPERTY(BlueprintAssignable, Category = "Twin|Registry")
	FTwinEntityRegistrySignature OnEntityUnregistered;

	FTwinEntityRegistryNativeSignature OnEntityRegisteredNative;
	FTwinEntityRegistryNativeSignature OnEntityUnregisteredNative;

	bool RegisterEntity(UTwinEntityComponent* Entity);
	void UnregisterEntity(UTwinEntityComponent* Entity);
	bool UpdateEntityId(UTwinEntityComponent* Entity, FName NewEntityId);

/**
 * 按实体 ID 查找当前 World 中的组件；未注册时返回空。
 * @param EntityId 目标实体的稳定标识。
 * @return 找不到目标或对象已经销毁时返回空。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Registry")
	UTwinEntityComponent* FindEntity(FName EntityId) const;

/**
 * 返回当前注册实体的快照，不应长期保存数组中的裸指针。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Registry")
	TArray<UTwinEntityComponent*> GetAllEntities() const;

/**
 * 按实体类型返回当前匹配项。
 * @param EntityType 用于筛选实体的类型名。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Registry")
	TArray<UTwinEntityComponent*> FindEntitiesByType(FName EntityType) const;

/**
 * 按 GameplayTag 返回当前匹配项。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Registry")
	TArray<UTwinEntityComponent*> FindEntitiesByTag(FGameplayTag EntityTag) const;

/**
 * 返回当前 World 已注册实体数量。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Registry")
	int32 GetEntityCount() const;

/**
 * 返回指定类型的已注册实体数量。
 * @param EntityType 用于筛选实体的类型名。
 */
	UFUNCTION(BlueprintPure, Category = "Twin|Registry")
	int32 GetEntityCountByType(FName EntityType) const;

private:
	void CleanupInvalidEntities() const;

	mutable TMap<FName, TWeakObjectPtr<UTwinEntityComponent>> Entities;
};
