// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinValidation.h"
#include "Containers/StringConv.h"
#include "Containers/StringView.h"
#include "Misc/StringBuilder.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
	bool IsValidValueType(ETwinValueType Type)
	{
		switch (Type)
		{
		case ETwinValueType::None:
		case ETwinValueType::Boolean:
		case ETwinValueType::Integer:
		case ETwinValueType::Number:
		case ETwinValueType::Text:
		case ETwinValueType::Vector:
		case ETwinValueType::Rotator:
			return true;
		default:
			return false;
		}
	}

	bool IsValidQuality(ETwinDataQuality Quality)
	{
		switch (Quality)
		{
		case ETwinDataQuality::Unknown:
		case ETwinDataQuality::Good:
		case ETwinDataQuality::Uncertain:
		case ETwinDataQuality::Bad:
			return true;
		default:
			return false;
		}
	}
}

bool TitanTwin::Validation::IsValidIdentifier(FStringView Value, int32 MaxChars, bool bAllowEmpty)
{
	if (MaxChars <= 0 || Value.Len() > MaxChars || (!bAllowEmpty && Value.IsEmpty()))
	{
		return false;
	}

	for (const TCHAR Character : Value)
	{
		if (FChar::IsControl(Character))
		{
			return false;
		}
	}

	return true;
}

bool TitanTwin::Validation::IsValidName(FName Value, int32 MaxChars, bool bAllowNone)
{
	if (Value.IsNone())
	{
		return bAllowNone;
	}

	TStringBuilder<NAME_SIZE> Text;
    Value.ToString(Text);
    return IsValidIdentifier(Text.ToView(), MaxChars, false);
}

bool TitanTwin::Validation::TryMakeName(
	const FString& Text, FName& OutName, int32 MaxChars, bool bAllowNone)
{
	OutName = NAME_None;
	if (!IsValidIdentifier(Text, FMath::Min(MaxChars, NAME_SIZE - 1), bAllowNone))
	{
		return false;
	}
	if (Text.IsEmpty() || Text.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		return bAllowNone;
	}

	// Existing names do not grow the pool. Only new external base names consume
	// this process-wide budget; it is never reset because FNames are never freed.
	static FCriticalSection NameBudgetMutex;
	static int32 NewExternalNames = 0;
	constexpr int32 MaximumNewExternalNames = 65536;
	FScopeLock Lock(&NameBudgetMutex);
	OutName = FName(*Text, FNAME_Find);
	if (!OutName.IsNone())
	{
		return true;
	}
	if (NewExternalNames >= MaximumNewExternalNames)
	{
		return false;
	}
	OutName = FName(*Text);
	++NewExternalNames;
	return !OutName.IsNone();
}

bool TitanTwin::Validation::IsValidValue(const FTwinValue& Value)
{
	if (!IsValidValueType(Value.Type))
	{
		return false;
	}

	switch (Value.Type)
	{
	case ETwinValueType::None:
	case ETwinValueType::Boolean:
	case ETwinValueType::Integer:
		return true;
	case ETwinValueType::Number:
		return FMath::IsFinite(Value.NumberValue);
	case ETwinValueType::Text:
		return Value.TextValue.Len() <= MaxTextChars;
	case ETwinValueType::Vector:
		return !Value.VectorValue.ContainsNaN();
	case ETwinValueType::Rotator:
		return !Value.RotatorValue.ContainsNaN();
	default:
		return false;
	}
}

bool TitanTwin::Validation::IsValidSample(const FTwinPropertySample& Sample)
{
	return Sample.Value.IsSet()
		&& IsValidValue(Sample.Value)
		&& IsValidQuality(Sample.Quality)
		&& IsValidName(Sample.Unit, MaxUnitChars, true)
		&& IsValidName(Sample.Source, MaxIdentifierChars, false);
}
