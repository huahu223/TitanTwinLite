// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "TwinDataSourceTypes.h"
#include "TwinMqttTypes.h"

class FTwinMqttPayloadParser final
{
public:
    static bool ParseMany(const FTwinMqttSubscriptionDefinition& Definition, FName SourceId,
        const FString& Topic,const FString& Payload,const FDateTime& MessageTimestampUtc,
        TArray<FTwinDataPoint>& OutPoints,FString& OutError);
	static bool Parse(
		const FTwinMqttSubscriptionDefinition& Definition,
		FName SourceId,
		const FString& Topic,
		const FString& Payload,
		const FDateTime& MessageTimestampUtc,
		FTwinDataPoint& OutPoint,
		FString& OutError);
};
