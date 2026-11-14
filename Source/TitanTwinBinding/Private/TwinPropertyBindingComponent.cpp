// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinPropertyBindingComponent.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/TextBlock.h"
#include "Internationalization/Text.h"
#include "TitanTwinBinding.h"
#include "TwinEntityComponent.h"
#include "TwinValidation.h"
#include "TwinReplaySafety.h"
#include "UObject/UnrealType.h"

namespace
{
	FString FormatValueAsString(
		const FTwinValue& Value,
		const FTwinPropertyBinding& Binding)
	{
		FString Result;
		switch (Value.Type)
		{
		case ETwinValueType::Boolean:
			Result = Value.BooleanValue ? TEXT("True") : TEXT("False");
			break;
		case ETwinValueType::Integer:
			Result = LexToString(Value.IntegerValue);
			break;
		case ETwinValueType::Number:
		{
			FNumberFormattingOptions Options;
			Options.SetUseGrouping(Binding.bUseGrouping);
			Options.SetMinimumFractionalDigits(FMath::Clamp(Binding.MinimumFractionalDigits, 0, 6));
			Options.SetMaximumFractionalDigits(FMath::Clamp(
				Binding.MaximumFractionalDigits,
				Binding.MinimumFractionalDigits,
				6));
			Result = FText::AsNumber(Value.NumberValue, &Options).ToString();
			break;
		}
		case ETwinValueType::Text:
			Result = Value.TextValue;
			break;
		case ETwinValueType::Vector:
			Result = Value.VectorValue.ToString();
			break;
		case ETwinValueType::Rotator:
			Result = Value.RotatorValue.ToString();
			break;
		default:
			break;
		}
		return Binding.TextPrefix + Result + Binding.TextSuffix;
	}

	bool IsWritableBindingProperty(const FProperty* Property)
	{
		if (!Property || Property->ArrayDim != 1
			|| !Property->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_Edit)
			|| Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_ConstParm)) return false;
		// A read-only widget reference is not reassigned: only its displayed text changes.
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (ObjectProperty && ObjectProperty->PropertyClass->IsChildOf(UTextBlock::StaticClass())) return true;
		return !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly | CPF_EditConst);
	}

	struct FScopedBindingValue
	{
        alignas(16) uint8 InlineStorage[256];
		const FProperty* Property;
		void* Data;
		FScopedBindingValue(const FProperty* InProperty, const UObject* Container)
			: Property(InProperty), Data(InProperty->GetSize() <= sizeof(InlineStorage) && InProperty->GetMinAlignment() <= 16
                ? InlineStorage : FMemory::Malloc(InProperty->GetSize(), InProperty->GetMinAlignment()))
		{
			Property->InitializeValue(Data);
			Property->GetValue_InContainer(Container, Data);
		}
		~FScopedBindingValue() { Property->DestroyValue(Data); if (Data != InlineStorage) FMemory::Free(Data); }
	};
}

TArray<FTwinPropertyBinding> UTwinPropertyBindingComponent::SnapshotBindings(FName SourceFilter)
{
	TSet<FGuid, DefaultKeyFuncs<FGuid>, TInlineSetAllocator<16>> Seen;
	for (FTwinPropertyBinding& Binding : Bindings)
	{
		if (!Binding.RuntimeBindingId.IsValid() || Seen.Contains(Binding.RuntimeBindingId))
			Binding.RuntimeBindingId = FGuid::NewGuid();
		Seen.Add(Binding.RuntimeBindingId);
	}
    if (SourceFilter.IsNone()) return Bindings;
    TArray<FTwinPropertyBinding> Snapshot;
    Snapshot.Reserve(1);
    for (const FTwinPropertyBinding& Binding : Bindings)
    {
        if (Binding.bEnabled && Binding.SourcePropertyName == SourceFilter)
        {
            Snapshot.Add(Binding);
        }
    }
    return Snapshot;
}

void UTwinPropertyBindingComponent::UpdateBindingStatus(const FGuid& Id, bool bApplied, const FString& Error)
{
	if (FTwinPropertyBinding* Current = Bindings.FindByPredicate(
		[&Id](const FTwinPropertyBinding& Item) { return Item.RuntimeBindingId == Id; }))
	{
		Current->LastError = Error;
		if (bApplied) ++Current->AppliedCount;
	}
}

UTwinPropertyBindingComponent::UTwinPropertyBindingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 0.5f;
}

void UTwinPropertyBindingComponent::BeginPlay()
{
	Super::BeginPlay();
	Rebind();
}

void UTwinPropertyBindingComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	Unbind();
	Super::EndPlay(EndPlayReason);
}

bool UTwinPropertyBindingComponent::Rebind()
{
	Unbind();
	AActor* Owner = GetOwner();
	UTwinEntityComponent* Entity = Owner
		? Owner->FindComponentByClass<UTwinEntityComponent>()
		: nullptr;
	if (!Entity)
	{
		UE_LOG(
			LogTitanTwinBinding,
			Warning,
			TEXT("%s has no TwinEntity component to bind."),
			*GetNameSafe(Owner));
		return false;
	}

	BoundEntity = Entity;
	Entity->OnPropertySampleChangedNative.AddUObject(
		this,
		&UTwinPropertyBindingComponent::HandlePropertySampleChanged);
	if (bApplyExistingValuesOnBeginPlay)
	{
		ApplyExistingValues();
	}
	return true;
}

void UTwinPropertyBindingComponent::Unbind()
{
	if (UTwinEntityComponent* Entity = BoundEntity.Get())
	{
		Entity->OnPropertySampleChangedNative.RemoveAll(this);
	}
	BoundEntity.Reset();
}

int32 UTwinPropertyBindingComponent::ApplyExistingValues()
{
	UTwinEntityComponent* Entity = BoundEntity.Get();
	if (!Entity)
	{
		return 0;
	}

	int32 Applied = 0;
	for (FTwinPropertyBinding Binding : SnapshotBindings())
	{
		if (!Binding.bEnabled || Binding.SourcePropertyName.IsNone())
		{
			continue;
		}
		if (ApplySample(Binding))
		{
			++Applied;
		}
	}
	return Applied;
}

bool UTwinPropertyBindingComponent::ValidateBindings()
{
	bool bAllValid = true;
	for (FTwinPropertyBinding Binding : SnapshotBindings())
	{
		Binding.LastError.Reset();
		UpdateBindingStatus(Binding.RuntimeBindingId, false, FString());
		if (!Binding.bEnabled)
		{
			continue;
		}
		if (Binding.SourcePropertyName.IsNone())
		{
			ReportFailure(Binding, TEXT("SourcePropertyName is empty."));
			bAllValid = false;
			continue;
		}
		if (Binding.TargetVariableName.IsNone())
		{
			ReportFailure(Binding, TEXT("TargetVariableName is empty."));
			bAllValid = false;
			continue;
		}
		UObject* TargetObject = ResolveTargetObject(Binding);
		if (!TargetObject)
		{
			ReportFailure(Binding, TEXT("The target object could not be resolved."));
			bAllValid = false;
			continue;
		}
		const FProperty* Property = FindFProperty<FProperty>(
			TargetObject->GetClass(),
			Binding.TargetVariableName);
		if (!IsWritableBindingProperty(Property))
		{
			ReportFailure(
				Binding,
				FString::Printf(
					TEXT("Target variable '%s' was not found or is not Blueprint-visible."),
					*Binding.TargetVariableName.ToString()));
			bAllValid = false;
		}
		else
		{
			FTwinValue Value; FString Error;
			const bool bHasValue = BoundEntity.IsValid() && BoundEntity->GetProperty(Binding.SourcePropertyName, Value);
			if (!bHasValue)
			{
				// Unknown source: validate only the target family. Never invent a source value.
				const auto* Struct = CastField<FStructProperty>(Property);
				const auto* Object = CastField<FObjectPropertyBase>(Property);
				const bool Supported = CastField<FBoolProperty>(Property) || CastField<FEnumProperty>(Property)
					|| CastField<FNumericProperty>(Property) || CastField<FStrProperty>(Property)
					|| CastField<FNameProperty>(Property) || CastField<FTextProperty>(Property)
					|| (Struct && (Struct->Struct == FTwinValue::StaticStruct() || Struct->Struct == TBaseStructure<FVector>::Get() || Struct->Struct == TBaseStructure<FRotator>::Get()))
					|| (Object && Object->PropertyClass->IsChildOf(UTextBlock::StaticClass()));
				Error = Supported ? TEXT("Source value is not available; compatibility has not been validated.") : TEXT("Unsupported target property type.");
				ReportFailure(Binding, Error); bAllValid = false;
			}
			else if (!WriteTargetVariable(TargetObject, Binding.TargetVariableName, Value, Binding, Error, true))
			{ ReportFailure(Binding, Error); bAllValid = false; }
		}
	}
	return bAllValid;
}

void UTwinPropertyBindingComponent::HandlePropertySampleChanged(
	UTwinEntityComponent* Entity,
	FName PropertyName,
	const FTwinPropertySample& Sample)
{
	if (Entity != BoundEntity.Get())
	{
		return;
	}
	for (FTwinPropertyBinding Binding : SnapshotBindings(PropertyName))
	{
		if (Binding.bEnabled && Binding.SourcePropertyName == PropertyName)
		{
			ApplySample(Binding);
		}
	}
}

bool UTwinPropertyBindingComponent::ApplyBinding(
	FTwinPropertyBinding& Binding,
	const FTwinValue& Value)
{
	if (!Bindings.ContainsByPredicate([&](const FTwinPropertyBinding& Current)
		{ return Current.RuntimeBindingId == Binding.RuntimeBindingId && Current.bEnabled; })) return false;
	UObject* TargetObject = ResolveTargetObject(Binding);
	if (!TargetObject)
	{
		ReportFailure(Binding, TEXT("The target object could not be resolved."));
		return false;
	}

	FString Error;
	const TWeakObjectPtr<UObject> WeakTarget(TargetObject);
	if (!Bindings.ContainsByPredicate([&](const FTwinPropertyBinding& Current)
		{ return Current.RuntimeBindingId == Binding.RuntimeBindingId && Current.bEnabled; })) return false;
	if (!WriteTargetVariable(
		TargetObject,
		Binding.TargetVariableName,
		Value,
		Binding,
		Error))
	{
		ReportFailure(Binding, Error);
		return false;
	}

	TargetObject = WeakTarget.Get();
	if (!TargetObject)
	{
		ReportFailure(Binding, TEXT("Binding target was destroyed during its setter."));
		return false;
	}

	Binding.LastError.Reset();
	++Binding.AppliedCount;
	UpdateBindingStatus(Binding.RuntimeBindingId, true, FString());
	OnBindingApplied.Broadcast(
		Binding.SourcePropertyName,
		TargetObject,
		Binding.TargetVariableName);
	return true;
}

UObject* UTwinPropertyBindingComponent::ResolveTargetObject(
	const FTwinPropertyBinding& Binding) const
{
	return ResolveBindingTarget(GetOwner(), Binding);
}

UObject* UTwinPropertyBindingComponent::ResolveBindingTarget(AActor* Owner, const FTwinPropertyBinding& Binding)
{
	if (!IsValid(Owner))
	{
		return nullptr;
	}
	if (Binding.TargetScope == ETwinBindingTargetScope::OwnerActor)
	{
		return Owner;
	}

	UActorComponent* Component = Binding.TargetComponent.GetComponent(Owner);
	if (!Component && !Binding.TargetComponentName.IsNone())
	{
		TInlineComponentArray<UActorComponent*> Components;
		Owner->GetComponents(Components);
		for (UActorComponent* Candidate : Components)
		{
			if (IsValid(Candidate)
				&& Candidate->GetFName() == Binding.TargetComponentName)
			{
				Component = Candidate;
				break;
			}
		}
	}
	if (!Component)
	{
		return nullptr;
	}
	switch (Binding.TargetScope)
	{
	case ETwinBindingTargetScope::ActorComponent:
		return Component;
	case ETwinBindingTargetScope::WidgetFromComponent:
		if (UWidgetComponent* WidgetComponent = Cast<UWidgetComponent>(Component))
		{
			WidgetComponent->InitWidget();
			return WidgetComponent->GetUserWidgetObject();
		}
		break;
	case ETwinBindingTargetScope::AnimInstanceFromComponent:
		if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Component))
		{
			return Mesh->GetAnimInstance();
		}
		break;
	default:
		break;
	}
	return nullptr;
}

bool UTwinPropertyBindingComponent::WriteTargetVariable(
	UObject* TargetObject,
	FName TargetVariableName,
	const FTwinValue& Value,
	const FTwinPropertyBinding& Binding,
	FString& OutError, bool bValidateOnly)
{
	check(IsInGameThread());
	OutError.Reset();
	if (!IsValid(TargetObject) || TargetVariableName.IsNone())
	{
		OutError = TEXT("Target object or variable name is empty.");
		return false;
	}
	FProperty* Property = FindFProperty<FProperty>(
		TargetObject->GetClass(),
		TargetVariableName);
	if (!IsWritableBindingProperty(Property))
	{
		OutError = FString::Printf(
			TEXT("Target variable '%s' was not found or is not Blueprint-visible."),
			*TargetVariableName.ToString());
		return false;
	}
	if (!TitanTwin::Validation::IsValidValue(Value))
	{
		OutError = TEXT("Binding value is invalid or non-finite.");
		return false;
	}
	FScopedBindingValue Temporary(Property, TargetObject);
	void* Address = Temporary.Data;
	auto Commit = [&]() { if (!bValidateOnly) Property->SetValue_InContainer(TargetObject, Address); return true; };

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (StructProperty->Struct == FTwinValue::StaticStruct())
		{
			StructProperty->Struct->CopyScriptStruct(Address, &Value);
			return Commit();
		}
		if (StructProperty->Struct == TBaseStructure<FVector>::Get()
			&& Value.Type == ETwinValueType::Vector)
		{
			*static_cast<FVector*>(Address) = Value.VectorValue;
			return Commit();
		}
		if (StructProperty->Struct == TBaseStructure<FRotator>::Get()
			&& Value.Type == ETwinValueType::Rotator)
		{
			*static_cast<FRotator*>(Address) = Value.RotatorValue;
			return Commit();
		}
	}
	else if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		if (Value.Type == ETwinValueType::Boolean)
		{
			BoolProperty->SetPropertyValue(Address, Value.BooleanValue);
			return Commit();
		}
	}
	else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		if (Value.Type == ETwinValueType::Integer
			&& EnumProperty->GetUnderlyingProperty()->CanHoldValue(Value.IntegerValue))
		{
			if (!EnumProperty->GetEnum()->IsValidEnumValueOrBitfield(Value.IntegerValue))
			{
				OutError = TEXT("Integer does not name a valid enum value.");
				return false;
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Address, Value.IntegerValue);
			return Commit();
		}
	}
	else if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
	{
		if (NumericProperty->IsFloatingPoint()
			&& (Value.Type == ETwinValueType::Number
				|| Value.Type == ETwinValueType::Integer))
		{
			const double Number = Value.Type == ETwinValueType::Number
				? Value.NumberValue
				: static_cast<double>(Value.IntegerValue);
			if (!FMath::IsFinite(Number)
				|| (CastField<FFloatProperty>(Property) && FMath::Abs(Number) > MAX_flt))
			{
				OutError = TEXT("Number cannot be represented by the target floating-point type.");
				return false;
			}
			NumericProperty->SetFloatingPointPropertyValue(Address, Number);
			return Commit();
		}
		if (NumericProperty->IsInteger()
			&& Value.Type == ETwinValueType::Integer
			&& NumericProperty->CanHoldValue(Value.IntegerValue))
		{
			if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property);
				ByteProperty && ByteProperty->Enum && !ByteProperty->Enum->IsValidEnumValueOrBitfield(Value.IntegerValue))
			{
				OutError = TEXT("Integer does not name a valid byte-enum value.");
				return false;
			}
			NumericProperty->SetIntPropertyValue(Address, Value.IntegerValue);
			return Commit();
		}
	}
	else if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
	{
		StringProperty->SetPropertyValue(Address, FormatValueAsString(Value, Binding));
		return Commit();
	}
	else if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		if (bValidateOnly) { if (!TitanTwin::Validation::IsValidIdentifier(FormatValueAsString(Value, Binding), TitanTwin::Validation::MaxIdentifierChars, true)) { OutError=TEXT("Formatted name is invalid or too long."); return false; } return true; }
		FName Name;
		if (!TitanTwin::Validation::TryMakeName(FormatValueAsString(Value, Binding), Name,
			TitanTwin::Validation::MaxIdentifierChars, true))
		{
			OutError = TEXT("Formatted name is invalid, too long or exceeds the external name budget.");
			return false;
		}
		NameProperty->SetPropertyValue(Address, Name);
		return Commit();
	}
	else if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		TextProperty->SetPropertyValue(Address, FText::FromString(FormatValueAsString(Value, Binding)));
		return Commit();
	}
	else if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		if (UTextBlock* TextBlock = Cast<UTextBlock>(
			ObjectProperty->GetObjectPropertyValue(Address)))
		{
			if (!bValidateOnly) TextBlock->SetText(FText::FromString(FormatValueAsString(Value, Binding)));
			return true;
		}
	}

	OutError = FString::Printf(
		TEXT("Twin value type %d is incompatible with target variable '%s' (%s)."),
		static_cast<int32>(Value.Type),
		*TargetVariableName.ToString(),
		*Property->GetClass()->GetName());
	return false;
}

void UTwinPropertyBindingComponent::ReportFailure(
	FTwinPropertyBinding& Binding,
	const FString& Error)
{
	Binding.LastError = Error;
	UpdateBindingStatus(Binding.RuntimeBindingId, false, Error);
	UE_LOG(
		LogTitanTwinBinding,
		Warning,
		TEXT("Binding '%s' -> '%s' failed on %s: %s"),
		*Binding.SourcePropertyName.ToString(),
		*Binding.TargetVariableName.ToString(),
		*GetNameSafe(GetOwner()),
		*Error);
	OnBindingFailed.Broadcast(Binding.SourcePropertyName, Error);
}

ETwinBindingSampleStatus UTwinPropertyBindingComponent::EvaluateSample(const FTwinPropertyBinding& Binding,
    const FTwinPropertySample& Sample, const FDateTime& Now, bool bReplay, FString& Reason)
{
    Reason.Reset();
    if (!FMath::IsFinite(Binding.MaximumSampleAgeSeconds) || Binding.MaximumSampleAgeSeconds < 0)
    { Reason=TEXT("Invalid sample expiry configuration"); return ETwinBindingSampleStatus::Stale; }
    if ((Sample.Quality == ETwinDataQuality::Uncertain && !Binding.bAcceptUncertainQuality)
        || (Sample.Quality != ETwinDataQuality::Good && Sample.Quality != ETwinDataQuality::Uncertain && !Binding.bAcceptBadOrUnknownQuality))
    { Reason=TEXT("Sample quality rejected; last applied value retained"); return ETwinBindingSampleStatus::BadQuality; }
    if (!bReplay && Binding.MaximumSampleAgeSeconds > 0 &&
        (Sample.Timestamp.GetTicks() <= 0 || (Now-Sample.Timestamp).GetTotalSeconds() > Binding.MaximumSampleAgeSeconds))
    { Reason=TEXT("Sample expired; last applied value retained"); return ETwinBindingSampleStatus::Stale; }
    return ETwinBindingSampleStatus::Ready;
}
bool UTwinPropertyBindingComponent::SetSampleStatus(const FTwinPropertyBinding& Binding, ETwinBindingSampleStatus Status, const FString& Reason)
{
    bool Changed=false;
    if (auto* Current=Bindings.FindByPredicate([&](const FTwinPropertyBinding& B){ return B.RuntimeBindingId==Binding.RuntimeBindingId; }))
    {
        Changed=Current->SampleStatus!=Status || Current->SampleStatusReason!=Reason;
        if(Status==ETwinBindingSampleStatus::Ready && Current->LastError==Current->SampleStatusReason)Current->LastError.Reset();
        Current->SampleStatus=Status; Current->SampleStatusReason=Reason;
        if(Status!=ETwinBindingSampleStatus::Ready)Current->LastError=Reason;
    }
    const bool HasCallback = Changed && OnSampleStatusChanged.IsBound();
    if (HasCallback) OnSampleStatusChanged.Broadcast(Binding.SourcePropertyName, Status, Reason);
    return HasCallback;
}
bool UTwinPropertyBindingComponent::ApplySample(const FTwinPropertyBinding& Binding)
{
    if (!IsValid(this) || !BoundEntity.IsValid()) return false;
    const auto* Current = Bindings.FindByPredicate([&](const FTwinPropertyBinding& Item)
    {
        return Item.RuntimeBindingId == Binding.RuntimeBindingId && Item.bEnabled;
    });
    if (!Current) return false;

    // A previous binding's callback may have changed this rule after the outer snapshot was taken.
    FTwinPropertyBinding CurrentBinding = *Current;
    FTwinPropertySample CurrentSample;
    if (!BoundEntity->GetPropertySample(CurrentBinding.SourcePropertyName, CurrentSample)) return false;
    FString Reason;
    auto Status = EvaluateSample(CurrentBinding, CurrentSample, FDateTime::UtcNow(),
        FTwinReplaySafety::IsLiveReplayActive(GetWorld()), Reason);
    if (SetSampleStatus(CurrentBinding, Status, Reason))
    {
        // This rule's status callback may also reconfigure it or destroy its owner.
        if (!IsValid(this) || !BoundEntity.IsValid()) return false;
        Current = Bindings.FindByPredicate([&](const FTwinPropertyBinding& Item)
        {
            return Item.RuntimeBindingId == Binding.RuntimeBindingId && Item.bEnabled;
        });
        if (!Current) return false;
        CurrentBinding = *Current;
        if (!BoundEntity->GetPropertySample(CurrentBinding.SourcePropertyName, CurrentSample)) return false;
        Status = EvaluateSample(CurrentBinding, CurrentSample, FDateTime::UtcNow(),
            FTwinReplaySafety::IsLiveReplayActive(GetWorld()), Reason);
    }
    return Status == ETwinBindingSampleStatus::Ready && ApplyBinding(CurrentBinding, CurrentSample.Value);
}
void UTwinPropertyBindingComponent::RefreshSampleStatuses()
{
    for (const auto& Binding: SnapshotBindings())
    {
        if (!Binding.bEnabled) continue;
        FTwinPropertySample Sample; FString Reason;
        auto Status=ETwinBindingSampleStatus::Missing;
        if (BoundEntity.IsValid() && BoundEntity->GetPropertySample(Binding.SourcePropertyName,Sample))
            Status=EvaluateSample(Binding,Sample,FDateTime::UtcNow(),FTwinReplaySafety::IsLiveReplayActive(GetWorld()),Reason);
        else Reason=TEXT("Source sample is missing; last applied value retained");
        SetSampleStatus(Binding,Status,Reason);
    }
}
void UTwinPropertyBindingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime,TickType,TickFunction);
    RefreshSampleStatuses();
}
