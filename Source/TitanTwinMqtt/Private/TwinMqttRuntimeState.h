// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "TwinDataSourceIngress.h"

#include <atomic>

struct FTwinMqttRuntimeState final
{
	TSharedPtr<FTwinDataSourceIngress, ESPMode::ThreadSafe> Ingress;
	FName SourceId;
	int32 MaxPayloadBytes = 1048576;
	std::atomic<bool> bAccepting{false};
	std::atomic<uint64> ActiveGeneration{0};
	std::atomic<int64> ConnectAttempts{0};
	std::atomic<int64> SuccessfulConnections{0};
	std::atomic<int64> Reconnects{0};
	std::atomic<int64> ReceivedMessages{0};
	std::atomic<int64> ParsedPoints{0};
	std::atomic<int64> ParseFailures{0};
	std::atomic<int64> OversizedMessages{0};
	std::atomic<int64> QueueRejectedMessages{0};
	std::atomic<int64> DroppedNativeMessages{0};
	std::atomic<int64> ProtocolErrors{0};
	std::atomic<int64> ReceivedBytes{0};
	std::atomic<int64> PublishedMessages{0};
	std::atomic<int64> PublishFailures{0};
};
