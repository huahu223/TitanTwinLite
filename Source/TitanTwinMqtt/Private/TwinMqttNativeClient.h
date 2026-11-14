// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "TwinMqttTypes.h"

#include <atomic>

class FTwinMqttTlsTransport;
class FRunnableThread;
class FSocket;
struct FTwinMqttRuntimeState;
struct FTwinMqttPersistentSession;
class FResolveInfo;

enum class ETwinMqttNativeEventType : uint8
{
	Ready,
	Message,
	Stopped
};

struct FTwinMqttNativeEvent
{
	ETwinMqttNativeEventType Type = ETwinMqttNativeEventType::Stopped;
	FString Topic;
	FString Payload;
	FDateTime Timestamp = FDateTime::UtcNow();
	FTwinMqttSubscriptionDefinition Definition;
	FString Error;
	int32 FailedSubscriptionCount = 0;
	int64 ReservedBytes = 0;
};

struct FTwinMqttNativeConfig
{
	FString Host;
	int32 Port = 1883;
	FString ClientId;
	FString Username;
	FString Password;
	bool bCleanSession = true;
	bool bUseTls = false;
    FString TlsCaFile;
	int32 KeepAliveSeconds = 30;
	float ConnectTimeoutSeconds = 10.0f;
	int32 MaxPayloadBytes = 1048576;
	int32 MaxPendingMessageEvents = 4096;
	int32 MaxPendingPublishes = 4096;
	int32 MaxPendingQos2Messages = 1024;
	float Qos2MessageTimeoutSeconds = 60.0f;
	int64 MaxRetainedBytes = 64 * 1024 * 1024;
	TSharedPtr<FTwinMqttPersistentSession, ESPMode::ThreadSafe> PersistentSession;
	TArray<FTwinMqttSubscriptionDefinition> Subscriptions;
	TSharedPtr<FTwinMqttRuntimeState, ESPMode::ThreadSafe> RuntimeState;
#if WITH_DEV_AUTOMATION_TESTS
	// Deterministic DNS timeout/stop tests without changing the machine's DNS.
	TFunction<FResolveInfo*()> TestResolverFactory;
#endif

	bool Validate(FString& OutError) const;
};

enum class ETwinMqttDecodeResult : uint8
{
	NeedMoreData,
	PacketReady,
	Malformed
};

struct FTwinMqttDecodedPacket
{
	uint8 Type = 0;
	uint8 Flags = 0;
	uint8 Qos = 0;
	uint16 PacketId = 0;
	int32 PayloadByteCount = 0;
	/** False when the payload is binary (not UTF-8 text). Such messages are acknowledged but never delivered. */
	bool bPayloadIsText = true;
	FString Topic;
	FString Payload;
	TArray<uint8> Body;
};

struct FTwinMqttPendingQos2
{
	FTwinMqttDecodedPacket Packet;
	double ReceivedAtSeconds = 0.0;
	int64 ReservedBytes = 0;
};

struct FTwinMqttPendingQos1
{
	TArray<uint8> Packet;
	int64 ReservedBytes = 0;
	int64 PublishSequence = 0;
};

/** Socket workers are joined before another worker touches the same session. */
struct FTwinMqttPersistentSession
{
	TMap<uint16, FTwinMqttPendingQos2> IncomingQos2;
	TMap<uint16, FTwinMqttPendingQos1> OutgoingQos1;
	uint16 NextPacketId = 2;
	int64 NextPublishSequence = 1;
	int64 RetainedBytes() const
	{
		int64 Bytes = 0;
		for (const auto& Pair : IncomingQos2) Bytes += Pair.Value.ReservedBytes;
		for (const auto& Pair : OutgoingQos1) Bytes += Pair.Value.ReservedBytes;
		return Bytes;
	}
};

class FTwinMqttPacketCodec final
{
public:
	static ETwinMqttDecodeResult Decode(
		TArrayView<const uint8> Buffer,
		int32 MaxPacketBytes,
		FTwinMqttDecodedPacket& OutPacket,
		int32& OutConsumedBytes,
		FString& OutError);

	static bool IsValidTopicFilter(const FString& Filter, FString* OutError = nullptr);
	static bool IsValidTopicName(const FString& Topic, FString* OutError = nullptr);
	static bool TopicMatchesFilter(const FString& Topic, const FString& Filter);

	static TArray<uint8> BuildConnect(
		const FString& ClientId,
		const FString& Username,
		const FString& Password,
		bool bCleanSession,
		uint16 KeepAliveSeconds);
	static TArray<uint8> BuildSubscribe(
		uint16 PacketId,
		const TArray<FTwinMqttSubscriptionDefinition>& Subscriptions);
	static TArray<uint8> BuildPublish(
		const FString& Topic,
		const FString& Payload,
		ETwinMqttQualityOfService QualityOfService,
		bool bRetain,
		uint16 PacketId = 0);
	static TArray<uint8> BuildSimplePacket(uint8 HeaderByte, uint16 PacketId = 0);
};

class FTwinMqttNativeClient final : public FRunnable
{
public:
	explicit FTwinMqttNativeClient(FTwinMqttNativeConfig InConfig);
	virtual ~FTwinMqttNativeClient() override;

	bool Start(FString& OutError);
	void StopAndWait();
	static void ShutdownResolvers();
	bool DequeueEvent(FTwinMqttNativeEvent& OutEvent);
	bool IsConnected() const;
	bool EnqueuePublish(
		const FString& Topic,
		const FString& Payload,
		ETwinMqttQualityOfService QualityOfService,
		bool bRetain,
		FString& OutError);

	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	bool ConnectSocket(FString& OutError);
	bool RunProtocol(FString& OutError);
	bool ReceiveIntoBuffer(double WaitSeconds, FString& OutError);
	bool SendPacket(const TArray<uint8>& Packet, FString& OutError);
	bool FlushOutgoingPublishes(FString& OutError);
	uint16 AllocatePacketId();
	bool ProcessAvailablePackets(bool& bOutReceivedConnAck, bool& bOutReceivedSubAck, FString& OutError);
	bool HandlePacket(const FTwinMqttDecodedPacket& Packet, bool& bOutReceivedConnAck, bool& bOutReceivedSubAck, FString& OutError);
	void HandlePublish(const FTwinMqttDecodedPacket& Packet, FString& OutError);
	bool DeliverMessage(const FTwinMqttDecodedPacket& Packet);
	bool EnqueueMessageEvent(FTwinMqttNativeEvent&& Event);
	void EnqueueControlEvent(FTwinMqttNativeEvent&& Event);
	void RecordProtocolError();
	bool PruneExpiredQos2Messages(double NowSeconds, FString& OutError);
	bool ReserveBytes(int64 Bytes);
	const FTwinMqttSubscriptionDefinition* FindDefinitionForTopic(const FString& Topic) const;
	void CloseSocket();
	void EnqueueStopped(const FString& Error);

	FTwinMqttNativeConfig Config;
	TSharedRef<FTwinMqttPersistentSession, ESPMode::ThreadSafe> Session;
	FRunnableThread* Thread = nullptr;
	FSocket* Socket = nullptr;
    TUniquePtr<FTwinMqttTlsTransport> Tls;
	TArray<uint8> ReceiveBuffer;
	using FPendingQos2Message = FTwinMqttPendingQos2;
	TMap<uint16, FPendingQos2Message>& PendingQos2Messages;
	struct FPendingPublish
	{
		FString Topic;
		FString Payload;
		ETwinMqttQualityOfService QualityOfService = ETwinMqttQualityOfService::AtMostOnce;
		bool bRetain = false;
		int64 ReservedBytes = 0;
	};
	TQueue<FPendingPublish, EQueueMode::Mpsc> PendingPublishes;
	TMap<uint16, FTwinMqttPendingQos1>& PendingOutboundQos1;
	TQueue<FTwinMqttNativeEvent, EQueueMode::Mpsc> Events;
	TArray<FTwinMqttNativeEvent> PreReadyMessages;
	std::atomic<int32> PendingMessageEventCount{0};
	std::atomic<int32> PendingPublishCount{0};
	std::atomic<int64> RetainedBytes{0};
	std::atomic<bool> bStopRequested{false};
	std::atomic<bool> bConnected{false};
	double LastInboundSeconds = 0.0;
	double LastOutboundSeconds = 0.0;
	double PingSentSeconds = 0.0;
	bool bPingOutstanding = false;
	bool bStoppedEventSent = false;
	int32 FailedSubscriptionCount = 0;
	uint16& NextPacketId;
	bool bReadyEventQueued = false;
};
