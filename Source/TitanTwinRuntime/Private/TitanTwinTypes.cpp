// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TitanTwinTypes.h"

FTwinValue FTwinValue::MakeBoolean(bool Value)
{
	FTwinValue Result;
	Result.Type = ETwinValueType::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FTwinValue FTwinValue::MakeInteger(int64 Value)
{
	FTwinValue Result;
	Result.Type = ETwinValueType::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FTwinValue FTwinValue::MakeNumber(double Value)
{
	FTwinValue Result;
	Result.Type = ETwinValueType::Number;
	Result.NumberValue = Value;
	return Result;
}

FTwinValue FTwinValue::MakeText(const FString& Value)
{
	FTwinValue Result;
	Result.Type = ETwinValueType::Text;
	Result.TextValue = Value;
	return Result;
}

FTwinValue FTwinValue::MakeVector(const FVector& Value)
{
	FTwinValue Result;
	Result.Type = ETwinValueType::Vector;
	Result.VectorValue = Value;
	return Result;
}

FTwinValue FTwinValue::MakeRotator(const FRotator& Value)
{
	FTwinValue Result;
	Result.Type = ETwinValueType::Rotator;
	Result.RotatorValue = Value;
	return Result;
}

bool FTwinValue::IsSet() const
{
	return Type != ETwinValueType::None;
}

bool FTwinValue::operator==(const FTwinValue& Other) const
{
	if (Type != Other.Type)
	{
		return false;
	}

	switch (Type)
	{
	case ETwinValueType::None:
		return true;
	case ETwinValueType::Boolean:
		return BooleanValue == Other.BooleanValue;
	case ETwinValueType::Integer:
		return IntegerValue == Other.IntegerValue;
	case ETwinValueType::Number:
		return NumberValue == Other.NumberValue;
	case ETwinValueType::Text:
		return TextValue == Other.TextValue;
	case ETwinValueType::Vector:
		return VectorValue == Other.VectorValue;
	case ETwinValueType::Rotator:
		return RotatorValue == Other.RotatorValue;
	default:
		return false;
	}
}

bool FTwinValue::IsNearlyEqual(const FTwinValue& Other, double Tolerance) const
{
	if (Type != Other.Type)
	{
		return false;
	}

	const double SafeTolerance = FMath::Max(0.0, Tolerance);
	switch (Type)
	{
	case ETwinValueType::None:
		return true;
	case ETwinValueType::Boolean:
		return BooleanValue == Other.BooleanValue;
	case ETwinValueType::Integer:
		return IntegerValue == Other.IntegerValue;
	case ETwinValueType::Number:
		return FMath::IsNearlyEqual(NumberValue, Other.NumberValue, SafeTolerance);
	case ETwinValueType::Text:
		return TextValue == Other.TextValue;
	case ETwinValueType::Vector:
		return VectorValue.Equals(Other.VectorValue, SafeTolerance);
	case ETwinValueType::Rotator:
		return RotatorValue.Equals(Other.RotatorValue, SafeTolerance);
	default:
		return false;
	}
}

bool FTwinPropertySample::IsEquivalentForNotification(const FTwinPropertySample& Other) const
{
	return Value.IsNearlyEqual(Other.Value)
		&& Timestamp == Other.Timestamp
		&& Quality == Other.Quality
		&& Unit == Other.Unit
		&& Source == Other.Source;
}

FTwinValue UTwinValueLibrary::MakeTwinBoolean(bool Value)
{
	return FTwinValue::MakeBoolean(Value);
}

FTwinValue UTwinValueLibrary::MakeTwinInteger(int64 Value)
{
	return FTwinValue::MakeInteger(Value);
}

FTwinValue UTwinValueLibrary::MakeTwinNumber(double Value)
{
	return FTwinValue::MakeNumber(Value);
}

FTwinValue UTwinValueLibrary::MakeTwinText(const FString& Value)
{
	return FTwinValue::MakeText(Value);
}

FTwinValue UTwinValueLibrary::MakeTwinVector(FVector Value)
{
	return FTwinValue::MakeVector(Value);
}

FTwinValue UTwinValueLibrary::MakeTwinRotator(FRotator Value)
{
	return FTwinValue::MakeRotator(Value);
}

FTwinPropertySample UTwinValueLibrary::MakeTwinSample(
	const FTwinValue& Value,
	FName Unit,
	ETwinDataQuality Quality,
	FName Source)
{
	FTwinPropertySample Result;
	Result.Value = Value;
	Result.Timestamp = FDateTime::UtcNow();
	Result.Unit = Unit;
	Result.Quality = Quality;
	Result.Source = Source;
	return Result;
}
