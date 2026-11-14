// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinMqttTypes.h"

bool FTwinMqttSubscriptionDefinition::IsValid(FString* OutReason) const
{
	auto Fail = [OutReason](const TCHAR* Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
		return false;
	};

	if (!bEnabled)
	{
		return Fail(TEXT("Subscription is disabled."));
	}
	if (TopicFilter.TrimStartAndEnd().IsEmpty())
	{
		return Fail(TEXT("Topic filter is required."));
	}
	if (PayloadFormat == ETwinMqttPayloadFormat::Json && JsonValueField.TrimStartAndEnd().IsEmpty())
	{
		return Fail(TEXT("JSON value field is required for JSON payloads."));
	}
    if (PayloadFormat != ETwinMqttPayloadFormat::Scalar && PayloadFormat != ETwinMqttPayloadFormat::Json && PayloadFormat != ETwinMqttPayloadFormat::JsonBatch)
        return Fail(TEXT("Unknown MQTT payload format."));
    if (PayloadFormat == ETwinMqttPayloadFormat::JsonBatch)
    {
        if (JsonEntityIdField.IsEmpty() || JsonFields.IsEmpty() || JsonFields.Num()>64 || MaxBatchPoints<1 || MaxBatchPoints>4096
            || MaxBatchBytes<1024 || MaxBatchBytes>16*1024*1024
            || !JsonAddressTemplate.Contains(TEXT("{entity}")) || !JsonAddressTemplate.Contains(TEXT("{property}"))
            || JsonAddressTemplate.Len()>512 || (JsonAddressTemplate.Contains(TEXT("{group}")) && JsonGroupIdField.IsEmpty()))
            return Fail(TEXT("Invalid JSON batch paths, template or limits."));
        TSet<FString> Properties;
        for (const auto& Field: JsonFields)
        {
            if (Field.ValuePath.IsEmpty() || Field.PropertyName.IsEmpty() || Field.PropertyName.Len()>128 || Properties.Contains(Field.PropertyName))
                return Fail(TEXT("JSON batch fields must have unique nonempty property names and value paths."));
            for (TCHAR C:Field.PropertyName) if (!(FChar::IsAlnum(C)||C=='_'||C=='-'||C=='.'||C==':')) return Fail(TEXT("Invalid batch property identifier."));
            Properties.Add(Field.PropertyName);
        }
    }
	return true;
}
