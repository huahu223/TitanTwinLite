// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinWorldSubsystem.h"

#include "TitanTwinRuntime.h"
#include "TwinEntityComponent.h"
#include "TwinValidation.h"

bool UTwinWorldSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::GamePreview;
}

void UTwinWorldSubsystem::Deinitialize()
{
	for (const TPair<FName, TWeakObjectPtr<UTwinEntityComponent>>& Pair : Entities)
	{
		if (UTwinEntityComponent* Entity = Pair.Value.Get())
		{
			Entity->bRegistered = false;
			Entity->RegisteredEntityId = NAME_None;
		}
	}

	Entities.Empty();
	OnEntityRegistered.Clear();
	OnEntityUnregistered.Clear();
	OnEntityRegisteredNative.Clear();
	OnEntityUnregisteredNative.Clear();
	Super::Deinitialize();
}

bool UTwinWorldSubsystem::RegisterEntity(UTwinEntityComponent* Entity)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("RegisterEntity must run on the game thread."))
		|| !IsValid(Entity))
	{
		return false;
	}

	CleanupInvalidEntities();
	Entity->EnsureIdentity();
	const FName EntityId = Entity->GetEntityId();

	if (!TitanTwin::Validation::IsValidName(EntityId))
	{
		return false;
	}

	if (const TWeakObjectPtr<UTwinEntityComponent>* ExistingEntity = Entities.Find(EntityId))
	{
		if (ExistingEntity->Get() != Entity)
		{
			UE_LOG(
				LogTitanTwin,
				Error,
				TEXT("Duplicate entity ID '%s' on '%s' and '%s'."),
				*EntityId.ToString(),
				*GetNameSafe(ExistingEntity->Get()),
				*GetNameSafe(Entity));
			return false;
		}

		Entity->bRegistered = true;
		Entity->RegisteredEntityId = EntityId;
		return true;
	}

	Entities.Add(EntityId, Entity);
	Entity->bRegistered = true;
	Entity->RegisteredEntityId = EntityId;
	OnEntityRegistered.Broadcast(Entity);
	OnEntityRegisteredNative.Broadcast(Entity);
	return true;
}

void UTwinWorldSubsystem::UnregisterEntity(UTwinEntityComponent* Entity)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("UnregisterEntity must run on the game thread."))
		|| !Entity)
	{
		return;
	}

	const FName RegisteredId = Entity->RegisteredEntityId;
	const TWeakObjectPtr<UTwinEntityComponent>* RegisteredEntity = Entities.Find(RegisteredId);
	// Commit before notifying: a callback may register the same component again.
	Entity->bRegistered = false;
	Entity->RegisteredEntityId = NAME_None;
	if (RegisteredEntity && RegisteredEntity->Get() == Entity)
	{
		Entities.Remove(RegisteredId);
		OnEntityUnregistered.Broadcast(Entity);
		OnEntityUnregisteredNative.Broadcast(Entity);
	}

}

bool UTwinWorldSubsystem::UpdateEntityId(UTwinEntityComponent* Entity, FName NewEntityId)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("UpdateEntityId must run on the game thread."))
		|| !IsValid(Entity)
		|| !TitanTwin::Validation::IsValidName(NewEntityId))
	{
		return false;
	}

	CleanupInvalidEntities();

	const FName PreviousId = Entity->RegisteredEntityId;
	const TWeakObjectPtr<UTwinEntityComponent>* RegisteredEntity = Entities.Find(PreviousId);
	if (!RegisteredEntity || RegisteredEntity->Get() != Entity)
	{
		UE_LOG(
			LogTitanTwin,
			Error,
			TEXT("Cannot change ID for unregistered entity '%s'."),
			*GetNameSafe(Entity));
		return false;
	}

	if (const TWeakObjectPtr<UTwinEntityComponent>* ExistingEntity = Entities.Find(NewEntityId))
	{
		if (ExistingEntity->Get() != Entity)
		{
			UE_LOG(LogTitanTwin, Error, TEXT("Entity ID '%s' is already in use."), *NewEntityId.ToString());
			return false;
		}
	}

	Entities.Remove(PreviousId);
	Entities.Add(NewEntityId, Entity);
	Entity->RegisteredEntityId = NewEntityId;
	return true;
}

UTwinEntityComponent* UTwinWorldSubsystem::FindEntity(FName EntityId) const
{
	if (const TWeakObjectPtr<UTwinEntityComponent>* Entity = Entities.Find(EntityId))
	{
		if (UTwinEntityComponent* ResolvedEntity = Entity->Get())
		{
			return ResolvedEntity;
		}

		// 高频查找只处理目标项；完整清理由枚举、计数和注册路径负责。
		Entities.Remove(EntityId);
	}

	return nullptr;
}

TArray<UTwinEntityComponent*> UTwinWorldSubsystem::GetAllEntities() const
{
	CleanupInvalidEntities();

	TArray<UTwinEntityComponent*> Result;
	Result.Reserve(Entities.Num());

	for (const TPair<FName, TWeakObjectPtr<UTwinEntityComponent>>& Pair : Entities)
	{
		if (UTwinEntityComponent* Entity = Pair.Value.Get())
		{
			Result.Add(Entity);
		}
	}

	Result.Sort([](const UTwinEntityComponent& A, const UTwinEntityComponent& B)
	{
		return A.GetEntityId().LexicalLess(B.GetEntityId());
	});
	return Result;
}

TArray<UTwinEntityComponent*> UTwinWorldSubsystem::FindEntitiesByType(FName EntityType) const
{
	TArray<UTwinEntityComponent*> Result;

	for (UTwinEntityComponent* Entity : GetAllEntities())
	{
		if (Entity->EntityType == EntityType)
		{
			Result.Add(Entity);
		}
	}

	return Result;
}

TArray<UTwinEntityComponent*> UTwinWorldSubsystem::FindEntitiesByTag(FGameplayTag EntityTag) const
{
	TArray<UTwinEntityComponent*> Result;

	if (!EntityTag.IsValid())
	{
		return Result;
	}

	for (UTwinEntityComponent* Entity : GetAllEntities())
	{
		if (Entity->EntityTags.HasTag(EntityTag))
		{
			Result.Add(Entity);
		}
	}

	return Result;
}

int32 UTwinWorldSubsystem::GetEntityCount() const
{
	CleanupInvalidEntities();
	return Entities.Num();
}

int32 UTwinWorldSubsystem::GetEntityCountByType(FName EntityType) const
{
	return FindEntitiesByType(EntityType).Num();
}

void UTwinWorldSubsystem::CleanupInvalidEntities() const
{
	for (auto Iterator = Entities.CreateIterator(); Iterator; ++Iterator)
	{
		if (!Iterator.Value().IsValid())
		{
			Iterator.RemoveCurrent();
		}
	}
}
