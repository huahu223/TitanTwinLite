// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinDataSourceTypes.h"

#include "TwinDataSourceMapping.h"
#include "TwinValidation.h"

namespace
{
	bool FailValidation(FString* OutReason, const TCHAR* Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
		return false;
	}
}

bool FTwinDataPoint::IsValid(FString* OutReason) const
{
	if (!TitanTwin::Validation::IsValidName(SourceId))
	{
		return FailValidation(OutReason, TEXT("SourceId is required and must be a valid identifier."));
	}
	if (Address.Len() > 4096)
	{
		return FailValidation(OutReason, TEXT("Raw address exceeds its input length limit."));
	}
	const FString TrimmedAddress = Address.TrimStartAndEnd();
	if (!TitanTwin::Validation::IsValidIdentifier(TrimmedAddress, 2048))
	{
		return FailValidation(OutReason, TEXT("Address is required and must not exceed 2048 characters."));
	}
	if (!Value.IsSet() || !TitanTwin::Validation::IsValidValue(Value))
	{
		return FailValidation(OutReason, TEXT("Value must be set, finite, and within size limits."));
	}
	if (Quality > ETwinDataQuality::Bad)
	{
		return FailValidation(OutReason, TEXT("Quality contains an invalid enum value."));
	}
	if (!TitanTwin::Validation::IsValidName(Unit, TitanTwin::Validation::MaxUnitChars, true))
	{
		return FailValidation(OutReason, TEXT("Unit is invalid or too long."));
	}
	if (TimestampUtc.GetTicks() > 0)
	{
		const FDateTime MinimumTimestamp(1970, 1, 1);
		if (TimestampUtc < MinimumTimestamp)
		{
			return FailValidation(OutReason, TEXT("TimestampUtc is outside the supported range."));
		}
		// 超前的时间戳默认不在这里拒绝，而是在游戏线程写入实体前钳到当前时间
		// （见 UTwinDataSourceSubsystem::ApplyPoint）。关闭 bClampFutureTimestamps 可恢复严格拒绝。
		if (!GetDefault<UTwinDataSourceSettings>()->bClampFutureTimestamps
			&& TimestampUtc > FDateTime::UtcNow()
				+ FTimespan::FromSeconds(TwinDataSourceLimits::MaximumFutureClockSkewSeconds))
		{
			return FailValidation(OutReason, TEXT("TimestampUtc is too far in the future."));
		}
	}
	return true;
}

bool FTwinDataSourceMapping::IsValid(FString* OutReason) const
{
	if (!TitanTwin::Validation::IsValidName(
		SourceId,
		TitanTwin::Validation::MaxIdentifierChars,
		true))
	{
		return FailValidation(OutReason, TEXT("SourceId is invalid or too long."));
	}
	if (!TitanTwin::Validation::IsValidIdentifier(Address.TrimStartAndEnd(), 2048))
	{
		return FailValidation(OutReason, TEXT("Address is required."));
	}
	if (!TitanTwin::Validation::IsValidName(EntityId))
	{
		return FailValidation(OutReason, TEXT("EntityId is required."));
	}
	if (!TitanTwin::Validation::IsValidName(PropertyName))
	{
		return FailValidation(OutReason, TEXT("PropertyName is required."));
	}
	if (bOverrideUnit
		&& !TitanTwin::Validation::IsValidName(Unit, TitanTwin::Validation::MaxUnitChars, true))
	{
		return FailValidation(OutReason, TEXT("Override unit is invalid or too long."));
	}
	return true;
}
