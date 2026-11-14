// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinMqttPayloadParser.h"

#include "TwinValidation.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedPtr<FJsonValue> FindJsonField(
		const TSharedPtr<FJsonObject>& Root,
		const FString& FieldPath)
	{
		if (!Root.IsValid() || FieldPath.IsEmpty())
		{
			return nullptr;
		}

		TArray<FString> Segments;
		FieldPath.ParseIntoArray(Segments, TEXT("."), true);
		TSharedPtr<FJsonObject> CurrentObject = Root;
		TSharedPtr<FJsonValue> CurrentValue;

		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			CurrentValue = CurrentObject->TryGetField(Segments[Index]);
			if (!CurrentValue.IsValid())
			{
				return nullptr;
			}

			if (Index + 1 < Segments.Num())
			{
				if (CurrentValue->Type != EJson::Object)
				{
					return nullptr;
				}
				CurrentObject = CurrentValue->AsObject();
			}
		}

		return CurrentValue;
	}

	bool ParseBooleanText(const FString& Input, bool& OutValue, bool bAllowNumericAliases)
	{
		if (Input.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
			(bAllowNumericAliases && Input == TEXT("1")))
		{
			OutValue = true;
			return true;
		}
		if (Input.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
			(bAllowNumericAliases && Input == TEXT("0")))
		{
			OutValue = false;
			return true;
		}
		return false;
	}

	bool IsStrictIntegerText(const FString& Input)
	{
		if (Input.IsEmpty())
		{
			return false;
		}

		int32 Index = 0;
		if (Input[0] == TEXT('+') || Input[0] == TEXT('-'))
		{
			Index = 1;
		}

		if (Index == Input.Len())
		{
			return false;
		}

		for (; Index < Input.Len(); ++Index)
		{
			if (!FChar::IsDigit(Input[Index]))
			{
				return false;
			}
		}

		return true;
	}

	bool ParseTextValue(
		const FString& Input,
		ETwinMqttValueType ValueType,
		FTwinValue& OutValue)
	{
		const FString Trimmed = Input.TrimStartAndEnd();
		switch (ValueType)
		{
		case ETwinMqttValueType::Boolean:
		{
			bool Value = false;
			if (!ParseBooleanText(Trimmed, Value, true))
			{
				return false;
			}
			OutValue = FTwinValue::MakeBoolean(Value);
			return true;
		}
		case ETwinMqttValueType::Integer:
		{
			int64 Value = 0;
			if (!LexTryParseString(Value, *Trimmed))
			{
				return false;
			}
			OutValue = FTwinValue::MakeInteger(Value);
			return true;
		}
		case ETwinMqttValueType::Number:
		{
			double Value = 0.0;
			if (!LexTryParseString(Value, *Trimmed) || !FMath::IsFinite(Value))
			{
				return false;
			}
			OutValue = FTwinValue::MakeNumber(Value);
			return true;
		}
		case ETwinMqttValueType::Text:
			OutValue = FTwinValue::MakeText(Input);
			return true;
		case ETwinMqttValueType::Auto:
		default:
		{
			bool BooleanValue = false;
			if (ParseBooleanText(Trimmed, BooleanValue, false))
			{
				OutValue = FTwinValue::MakeBoolean(BooleanValue);
				return true;
			}

			int64 IntegerValue = 0;
			if (IsStrictIntegerText(Trimmed)
				&& LexTryParseString(IntegerValue, *Trimmed))
			{
				OutValue = FTwinValue::MakeInteger(IntegerValue);
				return true;
			}

			double NumberValue = 0.0;
			if (LexTryParseString(NumberValue, *Trimmed) && FMath::IsFinite(NumberValue))
			{
				OutValue = FTwinValue::MakeNumber(NumberValue);
				return true;
			}

			OutValue = FTwinValue::MakeText(Input);
			return true;
		}
		}
	}

	bool JsonScalarToText(const TSharedPtr<FJsonValue>& JsonValue, FString& OutText)
	{
		if (!JsonValue.IsValid())
		{
			return false;
		}

		switch (JsonValue->Type)
		{
		case EJson::String:
			OutText = JsonValue->AsString();
			return true;
		case EJson::Boolean:
			OutText = JsonValue->AsBool() ? TEXT("true") : TEXT("false");
			return true;
		case EJson::Number:
			OutText = FString::Printf(TEXT("%.17g"), JsonValue->AsNumber());
			return true;
		default:
			return false;
		}
	}

	bool ParseJsonValue(
		const TSharedPtr<FJsonValue>& JsonValue,
		ETwinMqttValueType ValueType,
		FTwinValue& OutValue)
	{
		if (!JsonValue.IsValid())
		{
			return false;
		}

		if (ValueType == ETwinMqttValueType::Auto)
		{
			switch (JsonValue->Type)
			{
			case EJson::Boolean:
				OutValue = FTwinValue::MakeBoolean(JsonValue->AsBool());
				return true;
			case EJson::Number:
			{
				const double Value = JsonValue->AsNumber();
				if (!FMath::IsFinite(Value))
				{
					return false;
				}
				// Whole numbers become Integer, matching the Scalar Auto rule and the HTTP/Pixel adapters,
				// so they can bind to int/byte/enum variables.
				if (FMath::Abs(Value) <= 9007199254740991.0 && FMath::FloorToDouble(Value) == Value)
				{
					OutValue = FTwinValue::MakeInteger(static_cast<int64>(Value));
				}
				else
				{
					OutValue = FTwinValue::MakeNumber(Value);
				}
				return true;
			}
			case EJson::String:
				return ParseTextValue(JsonValue->AsString(), ValueType, OutValue);
			default:
				return false;
			}
		}

		FString Text;
		return JsonScalarToText(JsonValue, Text) && ParseTextValue(Text, ValueType, OutValue);
	}

	bool UnixTimeToDateTime(double Value, FDateTime& OutTimestamp)
	{
		// Values above 1e11 cannot be plausible Unix seconds (year 5138), so they are milliseconds.
		if (!FMath::IsFinite(Value) || Value <= 0.0) return false;
		const double Seconds = Value > 1.0e11 ? Value / 1000.0 : Value;
		if (Seconds > 253402300799.0) return false; // 9999-12-31
		OutTimestamp = FDateTime(1970, 1, 1) + FTimespan::FromSeconds(Seconds);
		return true;
	}

	// Accepts ISO-8601, ISO-8601 with a space instead of 'T', and Unix seconds or milliseconds
	// (as a JSON number or numeric string). Device payloads commonly use all of these.
	bool ParseJsonTimestamp(const TSharedPtr<FJsonValue>& Field, FDateTime& OutTimestamp)
	{
		if (!Field.IsValid()) return false;
		if (Field->Type == EJson::Number) return UnixTimeToDateTime(Field->AsNumber(), OutTimestamp);
		if (Field->Type != EJson::String) return false;
		const FString Text = Field->AsString().TrimStartAndEnd();
		if (Text.IsEmpty()) return false;
		if (FDateTime::ParseIso8601(*Text, OutTimestamp)) return true;
		bool bNumeric = true;
		for (const TCHAR C : Text) if (!FChar::IsDigit(C) && C != TEXT('.')) { bNumeric = false; break; }
		double Number = 0.0;
		if (bNumeric && LexTryParseString(Number, *Text)) return UnixTimeToDateTime(Number, OutTimestamp);
		int32 Space = INDEX_NONE;
		if (Text.Len() > 10 && Text.FindChar(TEXT(' '), Space) && Space == 10)
		{
			FString WithT = Text;
			WithT[Space] = TEXT('T');
			return FDateTime::ParseIso8601(*WithT, OutTimestamp);
		}
		return false;
	}

	bool ParseQuality(const FString& Text, ETwinDataQuality& OutQuality)
	{
		if (Text.Equals(TEXT("good"), ESearchCase::IgnoreCase))
		{
			OutQuality = ETwinDataQuality::Good;
			return true;
		}
		if (Text.Equals(TEXT("uncertain"), ESearchCase::IgnoreCase))
		{
			OutQuality = ETwinDataQuality::Uncertain;
			return true;
		}
		if (Text.Equals(TEXT("bad"), ESearchCase::IgnoreCase))
		{
			OutQuality = ETwinDataQuality::Bad;
			return true;
		}
		if (Text.Equals(TEXT("unknown"), ESearchCase::IgnoreCase))
		{
			OutQuality = ETwinDataQuality::Unknown;
			return true;
		}
		return false;
	}
}

bool FTwinMqttPayloadParser::Parse(
	const FTwinMqttSubscriptionDefinition& Definition,
	FName SourceId,
	const FString& Topic,
	const FString& Payload,
	const FDateTime& MessageTimestampUtc,
	FTwinDataPoint& OutPoint,
	FString& OutError)
{
	OutPoint = FTwinDataPoint();
	OutError.Reset();

	if (SourceId.IsNone() || Topic.IsEmpty() || Payload.IsEmpty())
	{
		OutError = TEXT("Source ID, topic and payload are required.");
		return false;
	}

	OutPoint.SourceId = SourceId;
	OutPoint.Address = Topic;
	OutPoint.TimestampUtc = MessageTimestampUtc.GetTicks() > 0
		? MessageTimestampUtc
		: FDateTime::UtcNow();
	OutPoint.Quality = ETwinDataQuality::Good;
	OutPoint.Unit = Definition.DefaultUnit;

	if (Definition.PayloadFormat == ETwinMqttPayloadFormat::Scalar)
	{
		if (!ParseTextValue(Payload, Definition.ValueType, OutPoint.Value))
		{
			OutError = TEXT("Scalar payload does not match the configured value type.");
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OutError = TEXT("Payload is not a valid JSON object.");
		return false;
	}

	const TSharedPtr<FJsonValue> ValueField = FindJsonField(JsonObject, Definition.JsonValueField);
	if (!ParseJsonValue(ValueField, Definition.ValueType, OutPoint.Value))
	{
		OutError = FString::Printf(
			TEXT("JSON value field '%s' is missing or has an incompatible type."),
			*Definition.JsonValueField);
		return false;
	}

	if (!Definition.JsonTimestampField.IsEmpty())
	{
		const TSharedPtr<FJsonValue> TimestampField = FindJsonField(JsonObject, Definition.JsonTimestampField);
		if (TimestampField.IsValid())
		{
			FDateTime ParsedTimestamp;
			if (!ParseJsonTimestamp(TimestampField, ParsedTimestamp))
			{
				OutError = TEXT("JSON timestamp must be ISO-8601 or Unix seconds/milliseconds.");
				return false;
			}
			OutPoint.TimestampUtc = ParsedTimestamp;
		}
	}

	if (!Definition.JsonQualityField.IsEmpty())
	{
		const TSharedPtr<FJsonValue> QualityField = FindJsonField(JsonObject, Definition.JsonQualityField);
		if (QualityField.IsValid())
		{
			FString QualityText;
			if (!JsonScalarToText(QualityField, QualityText)
				|| !ParseQuality(QualityText, OutPoint.Quality))
			{
				OutError = TEXT("JSON quality must be good, uncertain, bad or unknown.");
				return false;
			}
		}
	}

	if (!Definition.JsonUnitField.IsEmpty())
	{
		const TSharedPtr<FJsonValue> UnitField = FindJsonField(JsonObject, Definition.JsonUnitField);
		if (UnitField.IsValid())
		{
			FString UnitText;
			if (!JsonScalarToText(UnitField, UnitText))
			{
				OutError = TEXT("JSON unit must be a scalar value.");
				return false;
			}
			if (!TitanTwin::Validation::TryMakeName(
				UnitText, OutPoint.Unit, TitanTwin::Validation::MaxUnitChars, true))
			{
				OutError = TEXT("JSON unit is invalid or exceeds the external name budget.");
				return false;
			}
		}
	}

	return true;
}

bool FTwinMqttPayloadParser::ParseMany(const FTwinMqttSubscriptionDefinition& D, FName SourceId,
    const FString& Topic,const FString& Payload,const FDateTime& Timestamp,TArray<FTwinDataPoint>& OutPoints,FString& Error)
{
    OutPoints.Reset(); Error.Reset();
    if (D.PayloadFormat!=ETwinMqttPayloadFormat::JsonBatch)
    {
        FTwinDataPoint P; if (!Parse(D,SourceId,Topic,Payload,Timestamp,P,Error)) return false;
        OutPoints.Add(MoveTemp(P)); return true;
    }
    if (!D.IsValid(&Error) || SourceId.IsNone() || Topic.IsEmpty()) return false;
    // Reject excessive nesting before the recursive JSON reader sees untrusted input.
    int32 Depth=0; bool InString=false, Escaped=false;
    for (TCHAR C:Payload)
    {
        if (InString) { if(Escaped) Escaped=false; else if(C=='\\') Escaped=true; else if(C=='"') InString=false; continue; }
        if(C=='"') InString=true;
        else if(C=='{' || C=='[') { if(++Depth>32){Error=TEXT("JSON nesting exceeded 32 levels");return false;} }
        else if(C=='}' || C==']') --Depth;
    }
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Payload),Root) || !Root.IsValid())
    { Error=TEXT("JSON batch root must be an object"); return false; }
    auto Identifier=[](const TSharedPtr<FJsonValue>& V,FString& Out)
    {
        if (!V.IsValid() || (V->Type!=EJson::String && V->Type!=EJson::Number) || !JsonScalarToText(V,Out) || Out.IsEmpty() || Out.Len()>128) return false;
        for (TCHAR C:Out) if (!(FChar::IsAlnum(C)||C=='_'||C=='-'||C=='.'||C==':')) return false;
        return true;
    };
    FString Group;
    if (!D.JsonGroupIdField.IsEmpty() && !Identifier(FindJsonField(Root,D.JsonGroupIdField),Group))
    { Error=TEXT("Invalid or missing root group identifier"); return false; }
    TArray<TSharedPtr<FJsonValue>> Items;
    if (D.JsonItemsPath.IsEmpty()) Items.Add(MakeShared<FJsonValueObject>(Root));
    else
    {
        const auto List=FindJsonField(Root,D.JsonItemsPath);
        if(!List.IsValid()||List->Type!=EJson::Array){Error=TEXT("JSON items path must select an array");return false;}
        if(List->AsArray().Num()>D.MaxBatchPoints/D.JsonFields.Num()){Error=TEXT("JSON batch point limit exceeded");return false;}
        Items=List->AsArray();
    }
    if(Items.IsEmpty()){Error=TEXT("JSON device list is empty");return false;}
    TArray<FTwinDataPoint> Pending; TSet<FString> Addresses; int64 Bytes=0;
    for (int32 I=0;I<Items.Num();++I)
    {
        if(!Items[I].IsValid()||Items[I]->Type!=EJson::Object){Error=TEXT("Device entries must be objects");return false;}
        const auto Item=Items[I]->AsObject(); FString Entity;
        if(!Identifier(FindJsonField(Item,D.JsonEntityIdField),Entity)){Error=FString::Printf(TEXT("Device %d has no valid identifier"),I);return false;}
        auto Envelope=MakeShared<FJsonObject>(); Envelope->SetNumberField(TEXT("value"),0);
        auto CopyMeta=[&](const FString& Path,const TCHAR* Name)
        { if(Path.IsEmpty())return; auto V=FindJsonField(Item,Path); if(!V.IsValid())V=FindJsonField(Root,Path); if(V.IsValid())Envelope->SetField(Name,V); };
        CopyMeta(D.JsonTimestampField,TEXT("timestamp")); CopyMeta(D.JsonQualityField,TEXT("quality")); CopyMeta(D.JsonUnitField,TEXT("unit"));
        FString MetaJson; FJsonSerializer::Serialize(Envelope,TJsonWriterFactory<>::Create(&MetaJson));
        FTwinMqttSubscriptionDefinition MetaD; MetaD.PayloadFormat=ETwinMqttPayloadFormat::Json; MetaD.DefaultUnit=D.DefaultUnit;
        FTwinDataPoint Base;
        if(!Parse(MetaD,SourceId,Topic,MetaJson,Timestamp,Base,Error))return false;
        for(const auto& Field:D.JsonFields)
        {
            FTwinDataPoint P=Base;
            if(!ParseJsonValue(FindJsonField(Item,Field.ValuePath),Field.ValueType,P.Value))
            {Error=FString::Printf(TEXT("Device %d: missing or incompatible field %s"),I,*Field.ValuePath);return false;}
            if(!Field.Unit.IsNone())P.Unit=Field.Unit;
            P.Address=D.JsonAddressTemplate.Replace(TEXT("{entity}"),*Entity,ESearchCase::CaseSensitive)
                .Replace(TEXT("{property}"),*Field.PropertyName,ESearchCase::CaseSensitive).Replace(TEXT("{group}"),*Group,ESearchCase::CaseSensitive);
            if(P.Address.Contains(TEXT("{"))||P.Address.Contains(TEXT("}"))||!P.IsValid(&Error)||Addresses.Contains(P.Address))
            {if(Error.IsEmpty())Error=TEXT("Invalid template or duplicate batch address");return false;}
            Bytes+=sizeof(FTwinDataPoint)+P.Address.GetAllocatedSize()+P.Value.TextValue.GetAllocatedSize();
            if(Pending.Num()>=D.MaxBatchPoints||Bytes>D.MaxBatchBytes){Error=TEXT("JSON batch point/byte limit exceeded");return false;}
            Addresses.Add(P.Address); Pending.Add(MoveTemp(P));
        }
    }
    OutPoints=MoveTemp(Pending); return true;
}
