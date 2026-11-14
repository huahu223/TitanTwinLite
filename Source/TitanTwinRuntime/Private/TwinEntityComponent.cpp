// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinEntityComponent.h"

#include "Async/Async.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TitanTwinRuntime.h"
#include "TwinValidation.h"
#include "TwinWorldSubsystem.h"
#include "TwinEntitySampleQueue.h"
#include "Containers/Ticker.h"

namespace
{
	bool IsValidConnectionState(ETwinConnectionState State)
	{
		return State == ETwinConnectionState::Unknown
			|| State == ETwinConnectionState::Offline
			|| State == ETwinConnectionState::Online;
	}

	bool IsValidOperationalState(ETwinOperationalState State)
	{
		return State == ETwinOperationalState::Unknown
			|| State == ETwinOperationalState::Idle
			|| State == ETwinOperationalState::Active;
	}

	bool IsValidHealthState(ETwinHealthState State)
	{
		return State == ETwinHealthState::Unknown
			|| State == ETwinHealthState::Normal
			|| State == ETwinHealthState::Warning
			|| State == ETwinHealthState::Fault;
	}
}

UTwinEntityComponent::UTwinEntityComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	PendingSampleQueue = MakeShared<FTwinEntitySampleQueue, ESPMode::ThreadSafe>();
}

void UTwinEntityComponent::OnRegister()
{
	Super::OnRegister();
}

void UTwinEntityComponent::BeginPlay()
{
	Super::BeginPlay();
	bEndingPlay = false;
	{
		FScopeLock Lock(&PendingSampleQueue->Mutex);
		PendingSampleQueue->bAccepting = true;
	}

	EnsureIdentity();

	if (UWorld* World = GetWorld())
	{
		if (UTwinWorldSubsystem* Subsystem = World->GetSubsystem<UTwinWorldSubsystem>())
		{
			if (!Subsystem->RegisterEntity(this))
			{
				UE_LOG(
					LogTitanTwin,
					Error,
					TEXT("Failed to register entity '%s' owned by '%s'."),
					*EntityId.ToString(),
					*GetNameSafe(GetOwner()));
			}
		}
	}
}

void UTwinEntityComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	bReplayViewActive = false; bNextReplayMutation = false; LivePropertiesDuringReplay.Reset();
	++PropertyNotificationGeneration;
	PropertyNotifications.Reset(); PropertyNotificationBytes = 0;
	bPropertyNotificationsScheduled = false;
	PendingSampleQueue->Stop();
	if (bRegistered)
	{
		if (UWorld* World = GetWorld())
		{
			if (UTwinWorldSubsystem* Subsystem = World->GetSubsystem<UTwinWorldSubsystem>())
			{
				Subsystem->UnregisterEntity(this);
			}
		}
	}

	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void UTwinEntityComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const bool bEntityIdChanged = PropertyChangedEvent.GetPropertyName()
		== GET_MEMBER_NAME_CHECKED(UTwinEntityComponent, EntityId);
	const FName RequestedEntityId = EntityId;
	const FName ComponentName = GetFName();
	const TWeakObjectPtr<AActor> OwnerActor = GetOwner();

	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (!bEntityIdChanged)
	{
		return;
	}

	UTwinEntityComponent* EditedComponent = nullptr;
	if (AActor* Owner = OwnerActor.Get())
	{
		Owner->Modify();
		TInlineComponentArray<UTwinEntityComponent*> TwinComponents;
		Owner->GetComponents(TwinComponents);
		for (UTwinEntityComponent* Component : TwinComponents)
		{
			if (IsValid(Component) && Component->GetFName() == ComponentName)
			{
				EditedComponent = Component;
				break;
			}
		}

		if (!EditedComponent && TwinComponents.Num() == 1 && IsValid(TwinComponents[0]))
		{
			EditedComponent = TwinComponents[0];
		}
	}

	if (!EditedComponent && IsValid(this))
	{
		EditedComponent = this;
	}

	if (!EditedComponent)
	{
		return;
	}

	EditedComponent->Modify();
	if (RequestedEntityId.IsNone())
	{
		EditedComponent->EntityId = NAME_None;
		EditedComponent->bEntityIdWasGenerated = true;
	}
	else if (!EditedComponent->SetEntityId(RequestedEntityId))
	{
		return;
	}

	EditedComponent->MarkPackageDirty();
}

void UTwinEntityComponent::PostEditImport()
{
	Super::PostEditImport();

	// PIE/独立进程复制组件时，旧资产中的生成标记可能仍为 true，
	// 但用户已经为实例填写了稳定 EntityId。只有真正的 Twin_<GUID>
	// 自动标识才允许在复制后清空并重新生成，显式标识必须保留。
	const bool bHasGeneratedEntityId = bEntityIdWasGenerated
		&& EntityId.ToString().StartsWith(TEXT("Twin_"), ESearchCase::CaseSensitive);

	EntityGuid = FGuid::NewGuid();
	if (bHasGeneratedEntityId)
	{
		EntityId = NAME_None;
	}
	else
	{
		bEntityIdWasGenerated = false;
	}
}
#endif

FName UTwinEntityComponent::GetEntityId() const
{
	return EntityId;
}

FGuid UTwinEntityComponent::GetEntityGuid() const
{
	return EntityGuid;
}

bool UTwinEntityComponent::SetEntityId(FName NewEntityId)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("SetEntityId must run on the game thread.")))
	{
		return false;
	}

	if (!TitanTwin::Validation::IsValidName(NewEntityId))
	{
		UE_LOG(LogTitanTwin, Warning, TEXT("Rejected invalid entity ID '%s'."), *NewEntityId.ToString());
		return false;
	}

	if (EntityId == NewEntityId)
	{
		bEntityIdWasGenerated = false;
		return true;
	}

	if (bRegistered)
	{
		UWorld* World = GetWorld();
		UTwinWorldSubsystem* Subsystem = World ? World->GetSubsystem<UTwinWorldSubsystem>() : nullptr;
		if (!Subsystem || !Subsystem->UpdateEntityId(this, NewEntityId))
		{
			return false;
		}
	}

	const FName PreviousEntityId = EntityId;
	EntityId = NewEntityId;
	bEntityIdWasGenerated = false;
	OnEntityIdChanged.Broadcast(PreviousEntityId, EntityId);
	OnEntityIdChangedNative.Broadcast(this, PreviousEntityId, EntityId);
	return true;
}

void UTwinEntityComponent::SetEntityType(FName NewEntityType)
{
	if (!ensureMsgf(IsInGameThread(), TEXT("SetEntityType must run on the game thread.")))
	{
		return;
	}

	if (!TitanTwin::Validation::IsValidName(NewEntityType, TitanTwin::Validation::MaxIdentifierChars, true))
	{
		UE_LOG(LogTitanTwin, Warning, TEXT("Rejected invalid entity type '%s'."), *NewEntityType.ToString());
		return;
	}

	EntityType = NewEntityType;
}

bool UTwinEntityComponent::IsRegistered() const
{
	return bRegistered;
}

void UTwinEntityComponent::SetConnectionState(ETwinConnectionState NewState)
{
	const bool Historical = ConsumeReplayMutation();
	if (bReplayViewActive && !Historical)
	{
		if (IsInGameThread() && IsValidConnectionState(NewState) && LiveConnectionState!=NewState)
		{ const auto Previous=LiveConnectionState; LiveConnectionState=NewState; OnReplayLiveConnection.Broadcast(this, Previous, NewState); }
		return;
	}
	if (!ensureMsgf(IsInGameThread(), TEXT("SetConnectionState must run on the game thread."))
		|| !IsValidConnectionState(NewState)
		|| ConnectionState == NewState)
	{
		return;
	}

	const ETwinEntityStatus PreviousStatus = GetStatus();
	const ETwinConnectionState PreviousState = ConnectionState;
	ConnectionState = NewState;
	OnConnectionStateChanged.Broadcast(PreviousState, ConnectionState);
	OnConnectionStateChangedNative.Broadcast(this, PreviousState, ConnectionState);
	BroadcastLegacyStatusIfChanged(PreviousStatus);
}

ETwinConnectionState UTwinEntityComponent::GetConnectionState() const
{
	return ConnectionState;
}

void UTwinEntityComponent::SetOperationalState(ETwinOperationalState NewState)
{
	const bool Historical = ConsumeReplayMutation();
	if (bReplayViewActive && !Historical)
	{
		if (IsInGameThread() && IsValidOperationalState(NewState) && LiveOperationalState!=NewState)
		{ const auto Previous=LiveOperationalState; LiveOperationalState=NewState; OnReplayLiveOperational.Broadcast(this, Previous, NewState); }
		return;
	}
	if (!ensureMsgf(IsInGameThread(), TEXT("SetOperationalState must run on the game thread."))
		|| !IsValidOperationalState(NewState)
		|| OperationalState == NewState)
	{
		return;
	}

	const ETwinEntityStatus PreviousStatus = GetStatus();
	const ETwinOperationalState PreviousState = OperationalState;
	OperationalState = NewState;
	OnOperationalStateChanged.Broadcast(PreviousState, OperationalState);
	OnOperationalStateChangedNative.Broadcast(this, PreviousState, OperationalState);
	BroadcastLegacyStatusIfChanged(PreviousStatus);
}

ETwinOperationalState UTwinEntityComponent::GetOperationalState() const
{
	return OperationalState;
}

void UTwinEntityComponent::SetHealthState(ETwinHealthState NewState)
{
	const bool Historical = ConsumeReplayMutation();
	if (bReplayViewActive && !Historical)
	{
		if (IsInGameThread() && IsValidHealthState(NewState) && LiveHealthState!=NewState)
		{ const auto Previous=LiveHealthState; LiveHealthState=NewState; OnReplayLiveHealth.Broadcast(this, Previous, NewState); }
		return;
	}
	if (!ensureMsgf(IsInGameThread(), TEXT("SetHealthState must run on the game thread."))
		|| !IsValidHealthState(NewState)
		|| HealthState == NewState)
	{
		return;
	}

	const ETwinEntityStatus PreviousStatus = GetStatus();
	const ETwinHealthState PreviousState = HealthState;
	HealthState = NewState;
	OnHealthStateChanged.Broadcast(PreviousState, HealthState);
	OnHealthStateChangedNative.Broadcast(this, PreviousState, HealthState);
	BroadcastLegacyStatusIfChanged(PreviousStatus);
}

ETwinHealthState UTwinEntityComponent::GetHealthState() const
{
	return HealthState;
}

void UTwinEntityComponent::SetStatus(ETwinEntityStatus NewStatus)
{
	const bool Historical = ConsumeReplayMutation();
	if (bReplayViewActive && !Historical)
	{
		if (!IsInGameThread()) return;
		const auto PreviousConnection=LiveConnectionState; const auto PreviousOperational=LiveOperationalState; const auto PreviousHealth=LiveHealthState;
		switch (NewStatus)
		{
		case ETwinEntityStatus::Offline: LiveConnectionState=ETwinConnectionState::Offline; LiveOperationalState=ETwinOperationalState::Unknown; LiveHealthState=ETwinHealthState::Unknown; break;
		case ETwinEntityStatus::Idle: LiveConnectionState=ETwinConnectionState::Online; LiveOperationalState=ETwinOperationalState::Idle; LiveHealthState=ETwinHealthState::Normal; break;
		case ETwinEntityStatus::Active: LiveConnectionState=ETwinConnectionState::Online; LiveOperationalState=ETwinOperationalState::Active; LiveHealthState=ETwinHealthState::Normal; break;
		case ETwinEntityStatus::Warning: LiveConnectionState=ETwinConnectionState::Online; LiveHealthState=ETwinHealthState::Warning; break;
		case ETwinEntityStatus::Fault: LiveConnectionState=ETwinConnectionState::Online; LiveHealthState=ETwinHealthState::Fault; break;
		default: LiveConnectionState=ETwinConnectionState::Unknown; LiveOperationalState=ETwinOperationalState::Unknown; LiveHealthState=ETwinHealthState::Unknown; break;
		}
		if (PreviousConnection!=LiveConnectionState) OnReplayLiveConnection.Broadcast(this,PreviousConnection,LiveConnectionState);
		if (PreviousOperational!=LiveOperationalState) OnReplayLiveOperational.Broadcast(this,PreviousOperational,LiveOperationalState);
		if (PreviousHealth!=LiveHealthState) OnReplayLiveHealth.Broadcast(this,PreviousHealth,LiveHealthState);
		return;
	}
	const ETwinEntityStatus PreviousStatus = GetStatus();
	const ETwinConnectionState PreviousConnection = ConnectionState;
	const ETwinOperationalState PreviousOperational = OperationalState;
	const ETwinHealthState PreviousHealth = HealthState;

	switch (NewStatus)
	{
	case ETwinEntityStatus::Offline:
		ConnectionState = ETwinConnectionState::Offline;
		OperationalState = ETwinOperationalState::Unknown;
		HealthState = ETwinHealthState::Unknown;
		break;
	case ETwinEntityStatus::Idle:
		ConnectionState = ETwinConnectionState::Online;
		OperationalState = ETwinOperationalState::Idle;
		HealthState = ETwinHealthState::Normal;
		break;
	case ETwinEntityStatus::Active:
		ConnectionState = ETwinConnectionState::Online;
		OperationalState = ETwinOperationalState::Active;
		HealthState = ETwinHealthState::Normal;
		break;
	case ETwinEntityStatus::Warning:
		ConnectionState = ETwinConnectionState::Online;
		HealthState = ETwinHealthState::Warning;
		break;
	case ETwinEntityStatus::Fault:
		ConnectionState = ETwinConnectionState::Online;
		HealthState = ETwinHealthState::Fault;
		break;
	case ETwinEntityStatus::Unknown:
	default:
		ConnectionState = ETwinConnectionState::Unknown;
		OperationalState = ETwinOperationalState::Unknown;
		HealthState = ETwinHealthState::Unknown;
		break;
	}

	if (PreviousConnection != ConnectionState)
	{
		OnConnectionStateChanged.Broadcast(PreviousConnection, ConnectionState);
		OnConnectionStateChangedNative.Broadcast(this, PreviousConnection, ConnectionState);
	}
	if (PreviousOperational != OperationalState)
	{
		OnOperationalStateChanged.Broadcast(PreviousOperational, OperationalState);
		OnOperationalStateChangedNative.Broadcast(this, PreviousOperational, OperationalState);
	}
	if (PreviousHealth != HealthState)
	{
		OnHealthStateChanged.Broadcast(PreviousHealth, HealthState);
		OnHealthStateChangedNative.Broadcast(this, PreviousHealth, HealthState);
	}
	BroadcastLegacyStatusIfChanged(PreviousStatus);
}

ETwinEntityStatus UTwinEntityComponent::GetStatus() const
{
	if (ConnectionState == ETwinConnectionState::Offline)
	{
		return ETwinEntityStatus::Offline;
	}
	if (HealthState == ETwinHealthState::Fault)
	{
		return ETwinEntityStatus::Fault;
	}
	if (HealthState == ETwinHealthState::Warning)
	{
		return ETwinEntityStatus::Warning;
	}
	if (OperationalState == ETwinOperationalState::Active)
	{
		return ETwinEntityStatus::Active;
	}
	if (OperationalState == ETwinOperationalState::Idle)
	{
		return ETwinEntityStatus::Idle;
	}
	return ETwinEntityStatus::Unknown;
}

void UTwinEntityComponent::SetProperty(FName PropertyName, const FTwinValue& NewValue)
{
	FTwinPropertySample Sample;
	Sample.Value = NewValue;
	Sample.Timestamp = FDateTime::UtcNow();
	Sample.Quality = ETwinDataQuality::Good;
	Sample.Source = TEXT("Local");
	SetPropertySample(PropertyName, Sample);
}

bool UTwinEntityComponent::SetPropertySample(FName PropertyName, const FTwinPropertySample& NewSample)
{
	const bool Historical = ConsumeReplayMutation();
	if (!ensureMsgf(IsInGameThread(), TEXT("SetPropertySample must run on the game thread.")))
	{
		return false;
	}

	if (bEndingPlay || !IsValid(this) || !TitanTwin::Validation::IsValidName(PropertyName)
		|| !TitanTwin::Validation::IsValidSample(NewSample))
	{
		UE_LOG(LogTitanTwin, Warning, TEXT("Rejected an invalid property update on '%s'."), *EntityId.ToString());
		return false;
	}

	FTwinPropertySample NormalizedSample = NewSample;
	if (NormalizedSample.Timestamp == FDateTime())
	{
		NormalizedSample.Timestamp = FDateTime::UtcNow();
	}

	if (bReplayViewActive && !Historical)
	{
		LivePropertiesDuringReplay.Add(PropertyName, NormalizedSample);
		OnReplayLiveSample.Broadcast(this, PropertyName, NormalizedSample);
		return true;
	}
	const FTwinPropertySample* PreviousSample = Properties.Find(PropertyName);
	const bool bShouldNotify = !PreviousSample
		|| !PreviousSample->IsEquivalentForNotification(NormalizedSample);

	if (bShouldNotify)
	{
		FPropertyNotification Notification;
		Notification.Name = PropertyName;
		Notification.Sample = NormalizedSample;
		if (!QueuePropertyNotification(MoveTemp(Notification))) return false;
	}
	Properties.Add(PropertyName, NormalizedSample);
	DispatchPropertyNotifications();
	return true;
}

bool UTwinEntityComponent::QueuePropertyNotification(FPropertyNotification&& Notification)
{
	Notification.Bytes = sizeof(FPropertyNotification) + Notification.Sample.Value.TextValue.GetAllocatedSize()
		+ Notification.RemovedNames.GetAllocatedSize();
	if (PropertyNotifications.Num() >= 1024 || Notification.Bytes > 4 * 1024 * 1024 - PropertyNotificationBytes)
	{
		FScopeLock Lock(&PendingSampleQueue->Mutex);
		++PendingSampleQueue->Dropped;
		return false; // Reject before mutating the property; never silently lose its notification.
	}
	PropertyNotificationBytes += Notification.Bytes;
	PropertyNotifications.Add(MoveTemp(Notification));
	return true;
}

void UTwinEntityComponent::DispatchPropertyNotifications()
{
	if (bDispatchingPropertyNotifications || bEndingPlay || !IsValid(this)) return;
	TGuardValue<bool> DispatchGuard(bDispatchingPropertyNotifications, true);
	const double Start = FPlatformTime::Seconds();
	int32 Count = 0;
	while (!PropertyNotifications.IsEmpty() && !bEndingPlay && IsValid(this)
		&& Count++ < 256 && FPlatformTime::Seconds() - Start < 0.002)
	{
		// Never keep an array element reference across a user delegate.
		FPropertyNotification Notification = MoveTemp(PropertyNotifications[0]);
		PropertyNotifications.RemoveAt(0, 1, EAllowShrinking::No);
		PropertyNotificationBytes -= Notification.Bytes;
		if (Notification.bRemoved)
		{
			for (FName Name : Notification.RemovedNames)
			{
				OnPropertyRemoved.Broadcast(Name);
				if (bEndingPlay || !IsValid(this)) break;
				OnPropertyRemovedNative.Broadcast(this, Name);
				if (bEndingPlay || !IsValid(this)) break;
			}
		}
		else
		{
			OnPropertyChanged.Broadcast(Notification.Name, Notification.Sample.Value);
			if (bEndingPlay || !IsValid(this)) break;
			OnPropertySampleChanged.Broadcast(Notification.Name, Notification.Sample);
			if (bEndingPlay || !IsValid(this)) break;
			OnPropertySampleChangedNative.Broadcast(this, Notification.Name, Notification.Sample);
		}
	}
	if (!PropertyNotifications.IsEmpty() && !bEndingPlay && IsValid(this)) SchedulePropertyNotifications();
}

void UTwinEntityComponent::SchedulePropertyNotifications()
{
	if (bPropertyNotificationsScheduled) return;
	bPropertyNotificationsScheduled = true;
	const uint64 Generation = PropertyNotificationGeneration;
	const TWeakObjectPtr<UTwinEntityComponent> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, Generation](float)
	{
		if (auto* Entity = WeakThis.Get(); Entity && Entity->PropertyNotificationGeneration == Generation)
		{
			Entity->bPropertyNotificationsScheduled = false;
			Entity->DispatchPropertyNotifications();
		}
		return false;
	}));
}

void UTwinEntityComponent::EnqueuePropertySample(FName PropertyName, const FTwinPropertySample& NewSample)
{
	TryEnqueuePropertySample(PropertyName, NewSample);
}

bool UTwinEntityComponent::TryEnqueuePropertySample(FName PropertyName, const FTwinPropertySample& NewSample)
{
	if (!TitanTwin::Validation::IsValidName(PropertyName) || !TitanTwin::Validation::IsValidSample(NewSample))
	{
		FScopeLock Lock(&PendingSampleQueue->Mutex);
		++PendingSampleQueue->Dropped;
		return false;
	}
	if (IsInGameThread())
	{
		{ FScopeLock Lock(&PendingSampleQueue->Mutex); if (!PendingSampleQueue->bAccepting) return false; }
		return SetPropertySample(PropertyName, NewSample);
	}

	bool bSchedule = false;
	uint64 Generation = 0;
	const int64 Bytes = sizeof(FTwinEntitySampleQueue::FEntry) + NewSample.Value.TextValue.GetAllocatedSize();
	{
		FScopeLock Lock(&PendingSampleQueue->Mutex);
		if (!PendingSampleQueue->bAccepting || PendingSampleQueue->Count >= 1024
			|| Bytes > 4 * 1024 * 1024 - PendingSampleQueue->Bytes)
		{ ++PendingSampleQueue->Dropped; return false; }
		if (TwinPendingEntityBytes().Add(Bytes) + Bytes > 64 * 1024 * 1024)
		{ TwinPendingEntityBytes().Subtract(Bytes); ++PendingSampleQueue->Dropped; return false; }
		FTwinEntitySampleQueue::FEntry Entry{PropertyName, NewSample, Bytes};
		PendingSampleQueue->Queue.Enqueue(MoveTemp(Entry));
		++PendingSampleQueue->Count;
		PendingSampleQueue->Bytes += Bytes;
		Generation = PendingSampleQueue->Generation;
		bSchedule = !PendingSampleQueue->bScheduled;
		PendingSampleQueue->bScheduled = true;
	}
	const TWeakObjectPtr<UTwinEntityComponent> WeakThis(this);
	if (bSchedule) AsyncTask(ENamedThreads::GameThread, [WeakThis, Generation]()
	{
		if (UTwinEntityComponent* Entity = WeakThis.Get())
		{
			Entity->ScheduleSampleDrain(Generation);
		}
	});
	return true;
}

void UTwinEntityComponent::ScheduleSampleDrain(uint64 Generation)
{
	{ FScopeLock Lock(&PendingSampleQueue->Mutex);
		if (!PendingSampleQueue->bAccepting || PendingSampleQueue->Generation != Generation) return; }
	const TWeakObjectPtr<UTwinEntityComponent> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, Generation](float)
	{
		return WeakThis.IsValid() && WeakThis->DrainPendingSamples(Generation);
	}), 0.0f);
}

bool UTwinEntityComponent::DrainPendingSamples(uint64 Generation)
{
	const double Start = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < 64 && FPlatformTime::Seconds() - Start < 0.002; ++Index)
	{
		FTwinEntitySampleQueue::FEntry Entry;
		{
			FScopeLock Lock(&PendingSampleQueue->Mutex);
			if (!PendingSampleQueue->bAccepting || PendingSampleQueue->Generation != Generation) return false;
			if (!PendingSampleQueue->Queue.Dequeue(Entry)) break;
			--PendingSampleQueue->Count;
			PendingSampleQueue->Bytes -= Entry.Bytes;
			TwinPendingEntityBytes().Subtract(Entry.Bytes);
		}
		SetPropertySample(Entry.Property, Entry.Sample);
	}
	FScopeLock Lock(&PendingSampleQueue->Mutex);
	if (PendingSampleQueue->Generation != Generation || !PendingSampleQueue->bAccepting) return false;
	if (PendingSampleQueue->Count == 0) { PendingSampleQueue->bScheduled = false; return false; }
	return true;
}

int32 UTwinEntityComponent::GetPendingPropertySampleCount() const
{ FScopeLock Lock(&PendingSampleQueue->Mutex); return PendingSampleQueue->Count; }

int64 UTwinEntityComponent::GetDroppedPropertySampleCount() const
{ FScopeLock Lock(&PendingSampleQueue->Mutex); return PendingSampleQueue->Dropped; }

bool UTwinEntityComponent::GetProperty(FName PropertyName, FTwinValue& OutValue) const
{
	if (const FTwinPropertySample* Sample = Properties.Find(PropertyName))
	{
		OutValue = Sample->Value;
		return true;
	}

	return false;
}

bool UTwinEntityComponent::GetPropertySample(FName PropertyName, FTwinPropertySample& OutSample) const
{
	if (const FTwinPropertySample* Sample = Properties.Find(PropertyName))
	{
		OutSample = *Sample;
		return true;
	}

	return false;
}

TArray<FName> UTwinEntityComponent::GetPropertyNames() const
{
	TArray<FName> PropertyNames;
	Properties.GetKeys(PropertyNames);
	PropertyNames.Sort([](FName A, FName B)
	{
		return A.LexicalLess(B);
	});
	return PropertyNames;
}

bool UTwinEntityComponent::RemoveProperty(FName PropertyName)
{
	const bool Historical = ConsumeReplayMutation();
	if (bReplayViewActive && !Historical)
		{ if (!IsInGameThread() || bEndingPlay || LivePropertiesDuringReplay.Remove(PropertyName)==0) return false; OnReplayLiveRemoval.Broadcast(this, PropertyName); return true; }
	if (!ensureMsgf(IsInGameThread(), TEXT("RemoveProperty must run on the game thread.")))
	{
		return false;
	}

	if (bEndingPlay || !Properties.Contains(PropertyName))
	{
		return false;
	}

	FPropertyNotification Notification;
	Notification.bRemoved = true; Notification.RemovedNames.Add(PropertyName);
	if (!QueuePropertyNotification(MoveTemp(Notification))) return false;
	Properties.Remove(PropertyName);
	DispatchPropertyNotifications();
	return true;
}

void UTwinEntityComponent::ClearProperties()
{
	const bool Historical = ConsumeReplayMutation();
	if (bReplayViewActive && !Historical)
	{
		if (IsInGameThread() && !bEndingPlay)
		{
			TArray<FName> Removed; LivePropertiesDuringReplay.GetKeys(Removed); LivePropertiesDuringReplay.Reset();
			for (FName Name : Removed) { OnReplayLiveRemoval.Broadcast(this, Name); if (bEndingPlay || !IsValid(this)) break; }
		}
		return;
	}
	if (!ensureMsgf(IsInGameThread(), TEXT("ClearProperties must run on the game thread.")))
	{
		return;
	}

	if (bEndingPlay || Properties.IsEmpty()) return;
	FPropertyNotification Notification;
	Notification.bRemoved = true;
	Properties.GetKeys(Notification.RemovedNames);
	if (!QueuePropertyNotification(MoveTemp(Notification))) return;
	Properties.Empty();
	DispatchPropertyNotifications();
}

void UTwinEntityComponent::EnsureIdentity()
{
	if (!EntityGuid.IsValid())
	{
		EntityGuid = FGuid::NewGuid();
	}

	if (EntityId.IsNone())
	{
		EntityId = FName(*FString::Printf(
			TEXT("Twin_%s"),
			*EntityGuid.ToString(EGuidFormats::Digits)));
		bEntityIdWasGenerated = true;
	}
}

void UTwinEntityComponent::BroadcastLegacyStatusIfChanged(ETwinEntityStatus PreviousStatus)
{
	const ETwinEntityStatus NewStatus = GetStatus();
	if (PreviousStatus != NewStatus)
	{
		OnStatusChanged.Broadcast(PreviousStatus, NewStatus);
	}
}

bool UTwinEntityComponent::GetLivePropertySample(FName Name, FTwinPropertySample& OutSample) const
{
	const auto& Values = bReplayViewActive ? LivePropertiesDuringReplay : Properties;
	if (const auto* Sample = Values.Find(Name)) { OutSample=*Sample; return true; }
	return false;
}
void UTwinEntityComponent::BeginReplayView()
{
	if (bReplayViewActive) return;
	LivePropertiesDuringReplay=Properties;
	LiveConnectionState=ConnectionState; LiveOperationalState=OperationalState; LiveHealthState=HealthState;
	bReplayViewActive=true;
}
void UTwinEntityComponent::FinishReplayView()
{
	if (!bReplayViewActive) return;
	// Cancel historical notifications not yet dispatched before announcing live values.
	++PropertyNotificationGeneration; bPropertyNotificationsScheduled=false;
	PropertyNotifications.Reset(); PropertyNotificationBytes=0;
	auto Live = MoveTemp(LivePropertiesDuringReplay);
	const auto Connection=LiveConnectionState; const auto Operational=LiveOperationalState; const auto Health=LiveHealthState;
	bReplayViewActive=false; bNextReplayMutation=false;
	for (FName Name : GetPropertyNames())
	{
		if (!Live.Contains(Name)) RemoveProperty(Name);
		if (bEndingPlay || !IsValid(this)) return;
	}
	for (const auto& Pair : Live)
	{
		SetPropertySample(Pair.Key, Pair.Value);
		if (bEndingPlay || !IsValid(this)) return;
	}
	SetConnectionState(Connection);
	if (bEndingPlay || !IsValid(this)) return;
	SetOperationalState(Operational);
	if (bEndingPlay || !IsValid(this)) return;
	SetHealthState(Health);
}
