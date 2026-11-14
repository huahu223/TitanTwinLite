// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinMqttNativeClient.h"
#include "TwinMqttTlsTransport.h"
#include "SslModule.h"

#include "AddressInfoTypes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "IPAddressAsyncResolve.h"
#include "Misc/ScopeLock.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "TwinMqttRuntimeState.h"

namespace
{
	FCriticalSection ResolverMutex;
	TArray<TUniquePtr<FResolveInfo>> RetiredResolvers;
	int32 ActiveResolverCount = 0;
	bool bResolverShutdown = false;

	bool ReserveResolver()
	{
		FScopeLock Lock(&ResolverMutex);
		RetiredResolvers.RemoveAll([](const TUniquePtr<FResolveInfo>& Item)
		{
			if (!Item->IsComplete()) return false;
			--ActiveResolverCount;
			return true;
		});
		if (bResolverShutdown || ActiveResolverCount >= 4) return false;
		++ActiveResolverCount;
		return true;
	}
	void ReleaseResolver(TUniquePtr<FResolveInfo>&& Resolver)
	{
		FScopeLock Lock(&ResolverMutex);
		// Engine-owned resolver tasks may still be running after a client stops.
		// Keep their storage alive; no callback ever captures this plugin's worker.
		if (Resolver && !Resolver->IsComplete()) RetiredResolvers.Add(MoveTemp(Resolver));
		else --ActiveResolverCount;
	}
	constexpr uint8 ConnectPacketType = 1;
	constexpr uint8 ConnAckPacketType = 2;
	constexpr uint8 PublishPacketType = 3;
	constexpr uint8 PubAckPacketType = 4;
	constexpr uint8 PubRecPacketType = 5;
	constexpr uint8 PubRelPacketType = 6;
	constexpr uint8 PubCompPacketType = 7;
	constexpr uint8 SubscribePacketType = 8;
	constexpr uint8 SubAckPacketType = 9;
	constexpr uint8 UnsubscribePacketType = 10;
	constexpr uint8 PingReqPacketType = 12;
	constexpr uint8 PingRespPacketType = 13;
	constexpr uint8 DisconnectPacketType = 14;
	constexpr int32 MaxSupportedPayloadBytes = 16 * 1024 * 1024;
	constexpr int32 MaxSupportedSubscriptions = 1024;
	constexpr int32 MaxSubscribePacketBytes = 1024 * 1024;
	constexpr uint16 SubscribePacketId = 1;

	void AppendUInt16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>(Value & 0xff));
	}

	bool ReadUInt16(const TArray<uint8>& Bytes, int32& Offset, uint16& OutValue)
	{
		if (Offset < 0 || Offset + 2 > Bytes.Num())
		{
			return false;
		}
		OutValue = static_cast<uint16>((static_cast<uint16>(Bytes[Offset]) << 8) | Bytes[Offset + 1]);
		Offset += 2;
		return true;
	}

	bool IsValidMqttUtf8(TArrayView<const uint8> Bytes)
	{
		for (int32 Index = 0; Index < Bytes.Num();)
		{
			const uint8 Lead = Bytes[Index];
			uint32 CodePoint = 0;
			int32 SequenceLength = 0;
			if (Lead <= 0x7f)
			{
				CodePoint = Lead;
				SequenceLength = 1;
			}
			else if (Lead >= 0xc2 && Lead <= 0xdf)
			{
				CodePoint = Lead & 0x1f;
				SequenceLength = 2;
			}
			else if (Lead >= 0xe0 && Lead <= 0xef)
			{
				CodePoint = Lead & 0x0f;
				SequenceLength = 3;
			}
			else if (Lead >= 0xf0 && Lead <= 0xf4)
			{
				CodePoint = Lead & 0x07;
				SequenceLength = 4;
			}
			else
			{
				return false;
			}

			if (Index + SequenceLength > Bytes.Num())
			{
				return false;
			}
			for (int32 Continuation = 1; Continuation < SequenceLength; ++Continuation)
			{
				const uint8 Byte = Bytes[Index + Continuation];
				if ((Byte & 0xc0) != 0x80)
				{
					return false;
				}
				CodePoint = (CodePoint << 6) | (Byte & 0x3f);
			}

			const bool bOverlong = (SequenceLength == 2 && CodePoint < 0x80)
				|| (SequenceLength == 3 && CodePoint < 0x800)
				|| (SequenceLength == 4 && CodePoint < 0x10000);
			const bool bSurrogate = CodePoint >= 0xd800 && CodePoint <= 0xdfff;
			if (bOverlong || bSurrogate || CodePoint == 0 || CodePoint > 0x10ffff)
			{
				return false;
			}
			Index += SequenceLength;
		}
		return true;
	}

	bool AppendUtf8String(TArray<uint8>& Bytes, const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		if (Utf8.Length() > MAX_uint16)
		{
			return false;
		}
		AppendUInt16(Bytes, static_cast<uint16>(Utf8.Length()));
		if (Utf8.Length() > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}
		return true;
	}

	bool ReadUtf8String(const TArray<uint8>& Bytes, int32& Offset, FString& OutValue)
	{
		uint16 ByteLength = 0;
		if (!ReadUInt16(Bytes, Offset, ByteLength) || Offset + ByteLength > Bytes.Num())
		{
			return false;
		}
		const TArrayView<const uint8> Utf8Bytes(Bytes.GetData() + Offset, ByteLength);
		if (!IsValidMqttUtf8(Utf8Bytes))
		{
			return false;
		}
		if (ByteLength == 0)
		{
			OutValue.Reset();
		}
		else
		{
			const FUTF8ToTCHAR Converted(
				reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
				ByteLength);
			OutValue = FString(Converted.Length(), Converted.Get());
		}
		Offset += ByteLength;
		return true;
	}

	void AppendRemainingLength(TArray<uint8>& Bytes, int32 Length)
	{
		do
		{
			uint8 Encoded = static_cast<uint8>(Length % 128);
			Length /= 128;
			if (Length > 0)
			{
				Encoded |= 0x80;
			}
			Bytes.Add(Encoded);
		}
		while (Length > 0);
	}

	TArray<uint8> BuildPacket(uint8 HeaderByte, const TArray<uint8>& Body)
	{
		TArray<uint8> Packet;
		Packet.Reserve(Body.Num() + 5);
		Packet.Add(HeaderByte);
		AppendRemainingLength(Packet, Body.Num());
		Packet.Append(Body);
		return Packet;
	}

	bool DecodeRemainingLength(
		TArrayView<const uint8> Buffer,
		int32& OutValue,
		int32& OutLengthBytes,
		FString& OutError)
	{
		OutValue = 0;
		OutLengthBytes = 0;
		int32 Multiplier = 1;
		for (int32 Index = 1; Index < Buffer.Num() && OutLengthBytes < 4; ++Index)
		{
			const uint8 Encoded = Buffer[Index];
			OutValue += (Encoded & 0x7f) * Multiplier;
			++OutLengthBytes;
			if ((Encoded & 0x80) == 0)
			{
				const int32 MinimumValueForLength = OutLengthBytes == 1
					? 0
					: 1 << (7 * (OutLengthBytes - 1));
				if (OutValue < MinimumValueForLength)
				{
					OutError = TEXT("MQTT remaining-length field is not minimally encoded.");
					return false;
				}
				return true;
			}
			Multiplier *= 128;
		}

		if (OutLengthBytes == 4)
		{
			OutError = TEXT("MQTT remaining-length field exceeds four bytes.");
		}
		return false;
	}

	int32 QosToInt(ETwinMqttQualityOfService Qos)
	{
		switch (Qos)
		{
		case ETwinMqttQualityOfService::AtMostOnce:
			return 0;
		case ETwinMqttQualityOfService::ExactlyOnce:
			return 2;
		case ETwinMqttQualityOfService::AtLeastOnce:
		default:
			return 1;
		}
	}

	int32 CountCharacter(const FString& Value, TCHAR Character)
	{
		int32 Count = 0;
		for (const TCHAR Current : Value)
		{
			if (Current == Character)
			{
				++Count;
			}
		}
		return Count;
	}

	bool IsValidLengthPrefixedString(const FString& Value)
	{
		for (const TCHAR Character : Value)
		{
			if (Character == 0)
			{
				return false;
			}
		}
		const FTCHARToUTF8 Utf8(*Value);
		return Utf8.Length() <= MAX_uint16;
	}

	bool HasValidFixedHeaderFlags(uint8 PacketType, uint8 Flags)
	{
		if (PacketType == PublishPacketType)
		{
			return true;
		}
		const uint8 ExpectedFlags = PacketType == PubRelPacketType
			|| PacketType == SubscribePacketType
			|| PacketType == UnsubscribePacketType
			? 0x02
			: 0x00;
		return Flags == ExpectedFlags;
	}
}

ETwinMqttDecodeResult FTwinMqttPacketCodec::Decode(
	TArrayView<const uint8> Buffer,
	int32 MaxPacketBytes,
	FTwinMqttDecodedPacket& OutPacket,
	int32& OutConsumedBytes,
	FString& OutError)
{
	OutPacket = FTwinMqttDecodedPacket();
	OutConsumedBytes = 0;
	OutError.Reset();
	if (Buffer.Num() < 2)
	{
		return ETwinMqttDecodeResult::NeedMoreData;
	}

	int32 RemainingLength = 0;
	int32 RemainingLengthBytes = 0;
	if (!DecodeRemainingLength(Buffer, RemainingLength, RemainingLengthBytes, OutError))
	{
		return OutError.IsEmpty()
			? ETwinMqttDecodeResult::NeedMoreData
			: ETwinMqttDecodeResult::Malformed;
	}
	if (RemainingLength < 0 || RemainingLength > MaxPacketBytes)
	{
		OutError = FString::Printf(
			TEXT("MQTT packet body is %d bytes; limit is %d bytes."),
			RemainingLength,
			MaxPacketBytes);
		return ETwinMqttDecodeResult::Malformed;
	}

	const int32 HeaderBytes = 1 + RemainingLengthBytes;
	const int32 PacketBytes = HeaderBytes + RemainingLength;
	if (Buffer.Num() < PacketBytes)
	{
		return ETwinMqttDecodeResult::NeedMoreData;
	}

	const uint8 Header = Buffer[0];
	OutPacket.Type = Header >> 4;
	OutPacket.Flags = Header & 0x0f;
	if (OutPacket.Type == 0 || OutPacket.Type == 15)
	{
		OutError = TEXT("MQTT packet uses a reserved packet type.");
		return ETwinMqttDecodeResult::Malformed;
	}
	if (!HasValidFixedHeaderFlags(OutPacket.Type, OutPacket.Flags))
	{
		OutError = TEXT("MQTT packet uses invalid fixed-header flags.");
		return ETwinMqttDecodeResult::Malformed;
	}
	OutPacket.Body.Append(Buffer.GetData() + HeaderBytes, RemainingLength);
	OutConsumedBytes = PacketBytes;

	if (OutPacket.Type != PublishPacketType)
	{
		return ETwinMqttDecodeResult::PacketReady;
	}

	OutPacket.Qos = (Header >> 1) & 0x03;
	if (OutPacket.Qos == 3)
	{
		OutError = TEXT("MQTT PUBLISH packet uses reserved QoS value 3.");
		return ETwinMqttDecodeResult::Malformed;
	}
	if (OutPacket.Qos == 0 && (Header & 0x08) != 0)
	{
		OutError = TEXT("MQTT QoS 0 PUBLISH packets cannot set the DUP flag.");
		return ETwinMqttDecodeResult::Malformed;
	}

	int32 Offset = 0;
	if (!ReadUtf8String(OutPacket.Body, Offset, OutPacket.Topic) || OutPacket.Topic.IsEmpty())
	{
		OutError = TEXT("MQTT PUBLISH packet has an invalid topic.");
		return ETwinMqttDecodeResult::Malformed;
	}
	if (OutPacket.Topic.Contains(TEXT("+")) || OutPacket.Topic.Contains(TEXT("#")))
	{
		OutError = TEXT("MQTT PUBLISH topic names cannot contain wildcard characters.");
		return ETwinMqttDecodeResult::Malformed;
	}
	if (OutPacket.Qos > 0 && !ReadUInt16(OutPacket.Body, Offset, OutPacket.PacketId))
	{
		OutError = TEXT("MQTT PUBLISH packet is missing its packet identifier.");
		return ETwinMqttDecodeResult::Malformed;
	}
	if (OutPacket.Qos > 0 && OutPacket.PacketId == 0)
	{
		OutError = TEXT("MQTT PUBLISH packet identifier cannot be zero.");
		return ETwinMqttDecodeResult::Malformed;
	}
	if (Offset > OutPacket.Body.Num())
	{
		OutError = TEXT("MQTT PUBLISH packet has an invalid payload offset.");
		return ETwinMqttDecodeResult::Malformed;
	}

	const int32 PayloadBytes = OutPacket.Body.Num() - Offset;
	OutPacket.PayloadByteCount = PayloadBytes;
	if (PayloadBytes == 0)
	{
		OutPacket.Payload.Reset();
	}
	else
	{
		const TArrayView<const uint8> PayloadView(OutPacket.Body.GetData() + Offset, PayloadBytes);
		if (!IsValidMqttUtf8(PayloadView))
		{
			// MQTT payloads are arbitrary bytes. A binary payload is not a protocol error:
			// keep the connection, complete the QoS handshake and drop the message on delivery.
			OutPacket.bPayloadIsText = false;
			OutPacket.Payload.Reset();
			return ETwinMqttDecodeResult::PacketReady;
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(OutPacket.Body.GetData() + Offset),
			PayloadBytes);
		OutPacket.Payload = FString(Converted.Length(), Converted.Get());
	}
	return ETwinMqttDecodeResult::PacketReady;
}

bool FTwinMqttPacketCodec::IsValidTopicFilter(const FString& Filter, FString* OutError)
{
	auto Fail = [OutError](const TCHAR* Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
		return false;
	};

	if (Filter.IsEmpty())
	{
		return Fail(TEXT("MQTT topic filter cannot be empty."));
	}
	const FTCHARToUTF8 Utf8(*Filter);
	if (!IsValidLengthPrefixedString(Filter) || Utf8.Length() > MAX_uint16)
	{
		return Fail(TEXT("MQTT topic filter is too long or contains a null character."));
	}

	TArray<FString> Levels;
	Filter.ParseIntoArray(Levels, TEXT("/"), false);
	for (int32 Index = 0; Index < Levels.Num(); ++Index)
	{
		const FString& Level = Levels[Index];
		if (Level.Contains(TEXT("#")) && (Level != TEXT("#") || Index != Levels.Num() - 1))
		{
			return Fail(TEXT("MQTT '#' wildcard must occupy the final topic level."));
		}
		if (Level.Contains(TEXT("+")) && Level != TEXT("+"))
		{
			return Fail(TEXT("MQTT '+' wildcard must occupy an entire topic level."));
		}
	}
	return true;
}

bool FTwinMqttPacketCodec::IsValidTopicName(const FString& Topic, FString* OutError)
{
	if (!IsValidTopicFilter(Topic, OutError))
	{
		return false;
	}
	if (Topic.Contains(TEXT("#")) || Topic.Contains(TEXT("+")))
	{
		if (OutError)
		{
			*OutError = TEXT("MQTT publish topic names cannot contain wildcard characters.");
		}
		return false;
	}
	return true;
}

bool FTwinMqttPacketCodec::TopicMatchesFilter(const FString& Topic, const FString& Filter)
{
	if (Topic.StartsWith(TEXT("$"))
		&& (Filter.StartsWith(TEXT("#")) || Filter.StartsWith(TEXT("+"))))
	{
		return false;
	}
	TArray<FString> TopicLevels;
	TArray<FString> FilterLevels;
	Topic.ParseIntoArray(TopicLevels, TEXT("/"), false);
	Filter.ParseIntoArray(FilterLevels, TEXT("/"), false);

	int32 TopicIndex = 0;
	for (int32 FilterIndex = 0; FilterIndex < FilterLevels.Num(); ++FilterIndex)
	{
		const FString& Level = FilterLevels[FilterIndex];
		if (Level == TEXT("#"))
		{
			return FilterIndex == FilterLevels.Num() - 1;
		}
		if (TopicIndex >= TopicLevels.Num())
		{
			return false;
		}
		if (Level != TEXT("+") && Level != TopicLevels[TopicIndex])
		{
			return false;
		}
		++TopicIndex;
	}
	return TopicIndex == TopicLevels.Num();
}

bool FTwinMqttNativeConfig::Validate(FString& OutError) const
{
	OutError.Reset();
	auto Fail = [&OutError](const FString& Message)
	{
		OutError = Message;
		return false;
	};

	if (Host.TrimStartAndEnd().IsEmpty() || Host.Len() > 253)
	{
		return Fail(TEXT("MQTT broker host is required and must not exceed 253 characters."));
	}
	for (TCHAR C : Host)
		if (C > 127 || FChar::IsControl(C)) return Fail(TEXT("MQTT DNS hosts must use ASCII/punycode without control characters."));
	if (Port < 1 || Port > 65535)
	{
		return Fail(TEXT("MQTT broker port must be between 1 and 65535."));
	}
	if (!bCleanSession && ClientId.TrimStartAndEnd().IsEmpty())
	{
		return Fail(TEXT("A stable MQTT Client ID is required when Clean Session is disabled."));
	}
	if (!IsValidLengthPrefixedString(ClientId)
		|| !IsValidLengthPrefixedString(Username)
		|| !IsValidLengthPrefixedString(Password))
	{
		return Fail(TEXT("MQTT Client ID and credentials must each fit in a 65535-byte MQTT UTF-8 field."));
	}
	if (!Password.IsEmpty() && Username.IsEmpty())
	{
		return Fail(TEXT("MQTT password cannot be configured without a username."));
	}
	if (KeepAliveSeconds < 1 || KeepAliveSeconds > 65535)
	{
		return Fail(TEXT("MQTT keep-alive must be between 1 and 65535 seconds."));
	}
	if (!FMath::IsFinite(ConnectTimeoutSeconds) || ConnectTimeoutSeconds < 0.1f || ConnectTimeoutSeconds > 300.0f)
	{
		return Fail(TEXT("MQTT connect timeout must be between 0.1 and 300 seconds."));
	}
	if (MaxPayloadBytes < 1 || MaxPayloadBytes > MaxSupportedPayloadBytes)
	{
		return Fail(FString::Printf(TEXT("MQTT payload limit must be between 1 and %d bytes."), MaxSupportedPayloadBytes));
	}
	if (MaxPendingMessageEvents < 16 || MaxPendingMessageEvents > 65536)
	{
		return Fail(TEXT("MQTT pending-message limit must be between 16 and 65536."));
	}
	if (MaxPendingPublishes < 16 || MaxPendingPublishes > 65536)
	{
		return Fail(TEXT("MQTT pending-publish limit must be between 16 and 65536."));
	}
	if (MaxRetainedBytes < 65536 || MaxRetainedBytes > 268435456)
		return Fail(TEXT("MQTT retained byte budget is outside its supported range."));
	if (MaxPendingQos2Messages < 1 || MaxPendingQos2Messages > 65535)
	{
		return Fail(TEXT("MQTT pending QoS 2 limit must be between 1 and 65535."));
	}
	if (!FMath::IsFinite(Qos2MessageTimeoutSeconds)
		|| Qos2MessageTimeoutSeconds < 1.0f
		|| Qos2MessageTimeoutSeconds > 3600.0f)
	{
		return Fail(TEXT("MQTT QoS 2 timeout must be between 1 and 3600 seconds."));
	}
	if (Subscriptions.IsEmpty() || Subscriptions.Num() > MaxSupportedSubscriptions)
	{
		return Fail(FString::Printf(TEXT("MQTT requires between 1 and %d enabled subscriptions."), MaxSupportedSubscriptions));
	}
	TSet<FString> UniqueFilters;
	for (const FTwinMqttSubscriptionDefinition& Subscription : Subscriptions)
	{
		FString DefinitionError;
		if (!Subscription.bEnabled
			|| !Subscription.IsValid(&DefinitionError)
			|| !FTwinMqttPacketCodec::IsValidTopicFilter(Subscription.TopicFilter, &DefinitionError))
		{
			return Fail(DefinitionError.IsEmpty()
				? TEXT("MQTT native configuration contains an invalid subscription.")
				: DefinitionError);
		}
		if (UniqueFilters.Contains(Subscription.TopicFilter))
		{
			return Fail(FString::Printf(
				TEXT("Duplicate MQTT topic filter: %s"),
				*Subscription.TopicFilter));
		}
		UniqueFilters.Add(Subscription.TopicFilter);
	}
	if (FTwinMqttPacketCodec::BuildSubscribe(SubscribePacketId, Subscriptions).Num() > MaxSubscribePacketBytes)
	{
		return Fail(TEXT("Combined MQTT subscription packet exceeds the 1 MiB safety limit."));
	}
	return true;
}

TArray<uint8> FTwinMqttPacketCodec::BuildConnect(
	const FString& ClientId,
	const FString& Username,
	const FString& Password,
	bool bCleanSession,
	uint16 KeepAliveSeconds)
{
	TArray<uint8> Body;
	AppendUtf8String(Body, TEXT("MQTT"));
	Body.Add(4);
	uint8 ConnectFlags = bCleanSession ? 0x02 : 0x00;
	if (!Username.IsEmpty())
	{
		ConnectFlags |= 0x80;
	}
	if (!Password.IsEmpty())
	{
		ConnectFlags |= 0x40;
	}
	Body.Add(ConnectFlags);
	AppendUInt16(Body, KeepAliveSeconds);
	AppendUtf8String(Body, ClientId);
	if (!Username.IsEmpty())
	{
		AppendUtf8String(Body, Username);
	}
	if (!Password.IsEmpty())
	{
		AppendUtf8String(Body, Password);
	}
	return BuildPacket(ConnectPacketType << 4, Body);
}

TArray<uint8> FTwinMqttPacketCodec::BuildSubscribe(
	uint16 PacketId,
	const TArray<FTwinMqttSubscriptionDefinition>& Subscriptions)
{
	TArray<uint8> Body;
	AppendUInt16(Body, PacketId);
	for (const FTwinMqttSubscriptionDefinition& Subscription : Subscriptions)
	{
		AppendUtf8String(Body, Subscription.TopicFilter);
		Body.Add(static_cast<uint8>(QosToInt(Subscription.QualityOfService)));
	}
	return BuildPacket(static_cast<uint8>((SubscribePacketType << 4) | 0x02), Body);
}

TArray<uint8> FTwinMqttPacketCodec::BuildPublish(
	const FString& Topic,
	const FString& Payload,
	ETwinMqttQualityOfService QualityOfService,
	bool bRetain,
	uint16 PacketId)
{
	const int32 Qos = QosToInt(QualityOfService);
	if (!IsValidTopicName(Topic)
		|| (Qos > 0 && PacketId == 0))
	{
		return {};
	}

	TArray<uint8> Body;
	if (!AppendUtf8String(Body, Topic))
	{
		return {};
	}
	if (Qos > 0)
	{
		AppendUInt16(Body, PacketId);
	}
	const FTCHARToUTF8 PayloadUtf8(*Payload);
	if (PayloadUtf8.Length() > 0)
	{
		Body.Append(
			reinterpret_cast<const uint8*>(PayloadUtf8.Get()),
			PayloadUtf8.Length());
	}

	const uint8 Header = static_cast<uint8>(
		(PublishPacketType << 4)
		| ((Qos & 0x03) << 1)
		| (bRetain ? 0x01 : 0x00));
	return BuildPacket(Header, Body);
}

TArray<uint8> FTwinMqttPacketCodec::BuildSimplePacket(uint8 HeaderByte, uint16 PacketId)
{
	TArray<uint8> Body;
	if (PacketId != 0)
	{
		AppendUInt16(Body, PacketId);
	}
	return BuildPacket(HeaderByte, Body);
}

FTwinMqttNativeClient::FTwinMqttNativeClient(FTwinMqttNativeConfig InConfig)
	: Config(MoveTemp(InConfig))
	, Session(Config.PersistentSession.IsValid() ? Config.PersistentSession.ToSharedRef() : MakeShared<FTwinMqttPersistentSession, ESPMode::ThreadSafe>())
	, PendingQos2Messages(Session->IncomingQos2)
	, PendingOutboundQos1(Session->OutgoingQos1)
	, NextPacketId(Session->NextPacketId)
{
	if (Config.bCleanSession) { PendingQos2Messages.Reset(); PendingOutboundQos1.Reset(); NextPacketId = 2; }
	RetainedBytes.store(Session->RetainedBytes());
	PendingPublishCount.store(PendingOutboundQos1.Num());
}

FTwinMqttNativeClient::~FTwinMqttNativeClient()
{
	StopAndWait();
}

bool FTwinMqttNativeClient::Start(FString& OutError)
{
	if (Thread)
	{
		OutError = TEXT("MQTT client thread is already running.");
		return false;
	}
	if (!Config.Validate(OutError))
	{
		return false;
	}
	if (RetainedBytes.load() > Config.MaxRetainedBytes || PendingOutboundQos1.Num() > Config.MaxPendingPublishes)
	{
		OutError = TEXT("Persisted MQTT session exceeds the current queue limits.");
		return false;
	}
	if (Config.bUseTls) FSslModule::Get(); // Load engine SSL module before creating the worker.
	bStopRequested.store(false, std::memory_order_release);
	bStoppedEventSent = false;
	Thread = FRunnableThread::Create(this, TEXT("TitanTwinMqttClient"), 0, TPri_Normal);
	if (!Thread)
	{
		OutError = TEXT("Could not create the Titan Twin MQTT worker thread.");
		return false;
	}
	return true;
}

void FTwinMqttNativeClient::StopAndWait()
{
	Stop();
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	CloseSocket();
}

void FTwinMqttNativeClient::ShutdownResolvers()
{
	TArray<TUniquePtr<FResolveInfo>> Pending;
	{
		FScopeLock Lock(&ResolverMutex);
		bResolverShutdown = true;
		Pending = MoveTemp(RetiredResolvers);
	}
	// Module shutdown only: normal Disconnect never waits for a DNS task.
	for (const auto& Resolver : Pending)
		while (!Resolver->IsComplete()) FPlatformProcess::Sleep(0.01f);
}

bool FTwinMqttNativeClient::DequeueEvent(FTwinMqttNativeEvent& OutEvent)
{
	if (!Events.Dequeue(OutEvent))
	{
		return false;
	}
	if (OutEvent.Type == ETwinMqttNativeEventType::Message)
	{
		PendingMessageEventCount.fetch_sub(1, std::memory_order_acq_rel);
		RetainedBytes.fetch_sub(OutEvent.ReservedBytes);
	}
	return true;
}

bool FTwinMqttNativeClient::IsConnected() const
{
	return bConnected.load(std::memory_order_acquire);
}

bool FTwinMqttNativeClient::EnqueuePublish(
	const FString& Topic,
	const FString& Payload,
	ETwinMqttQualityOfService QualityOfService,
	bool bRetain,
	FString& OutError)
{
	OutError.Reset();
	if (!IsConnected())
	{
		OutError = TEXT("MQTT client is not connected.");
		return false;
	}
	if (QualityOfService == ETwinMqttQualityOfService::ExactlyOnce)
	{
		OutError = TEXT("Outbound MQTT QoS 2 is not supported; use QoS 0 or QoS 1.");
		return false;
	}
	if (!FTwinMqttPacketCodec::IsValidTopicName(Topic, &OutError))
	{
		return false;
	}
	const FTCHARToUTF8 PayloadUtf8(*Payload);
	if (PayloadUtf8.Length() > Config.MaxPayloadBytes)
	{
		OutError = TEXT("MQTT publish payload exceeds the configured safety limit.");
		return false;
	}

	int32 PendingCount = PendingPublishCount.load(std::memory_order_acquire);
	while (PendingCount < Config.MaxPendingPublishes)
	{
		if (PendingPublishCount.compare_exchange_weak(
			PendingCount,
			PendingCount + 1,
			std::memory_order_acq_rel,
			std::memory_order_acquire))
		{
				const int64 Bytes = sizeof(FPendingPublish) + Topic.GetAllocatedSize() + Payload.GetAllocatedSize();
				if (!ReserveBytes(Bytes))
				{
					PendingPublishCount.fetch_sub(1);
					OutError = TEXT("MQTT retained byte budget is full.");
					return false;
				}
			FPendingPublish Request;
			Request.Topic = Topic;
			Request.Payload = Payload;
			Request.QualityOfService = QualityOfService;
			Request.bRetain = bRetain;
				Request.ReservedBytes = Bytes;
			PendingPublishes.Enqueue(MoveTemp(Request));
			return true;
		}
	}
	OutError = TEXT("MQTT pending-publish queue is full.");
	return false;
}

uint32 FTwinMqttNativeClient::Run()
{
	FString Error;
	if (!ConnectSocket(Error) || !RunProtocol(Error))
	{
		if (!bStopRequested.load(std::memory_order_acquire))
		{
			EnqueueStopped(Error.IsEmpty() ? TEXT("MQTT connection closed unexpectedly.") : Error);
		}
	}
	bConnected.store(false, std::memory_order_release);
	CloseSocket();
	return 0;
}

void FTwinMqttNativeClient::Stop()
{
	bStopRequested.store(true, std::memory_order_release);
}

bool FTwinMqttNativeClient::ConnectSocket(FString& OutError)
{
	const double Deadline = FPlatformTime::Seconds() + FMath::Max(0.1, static_cast<double>(Config.ConnectTimeoutSeconds));

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		OutError = TEXT("No platform socket subsystem is available.");
		return false;
	}

	// The engine's numeric parser logs a warning for normal DNS names.
	bool bNumericHost = true;
	for (TCHAR C : Config.Host) if (!FChar::IsDigit(C) && C != TEXT('.')) { bNumericHost = false; break; }
	TSharedPtr<FInternetAddr> Address;
	if (bNumericHost || Config.Host.Contains(TEXT(":"))) Address = SocketSubsystem->GetAddressFromString(Config.Host);
	if (!Address.IsValid() || !Address->IsValid())
	{
		if (!ReserveResolver())
		{
			OutError = TEXT("Previous MQTT DNS lookups are still pending; resolver capacity is bounded.");
			return false;
		}
		TUniquePtr<FResolveInfo> Resolver;
#if WITH_DEV_AUTOMATION_TESTS
		if (Config.TestResolverFactory) Resolver.Reset(Config.TestResolverFactory());
		else
#endif
		Resolver.Reset(SocketSubsystem->GetHostByName(TCHAR_TO_ANSI(*Config.Host)));
		while (Resolver && !Resolver->IsComplete() && !bStopRequested.load(std::memory_order_acquire)
			&& FPlatformTime::Seconds() < Deadline)
			FPlatformProcess::Sleep(0.01f);
		if (!Resolver || !Resolver->IsComplete() || Resolver->GetErrorCode() != SE_NO_ERROR)
		{
			OutError = bStopRequested.load(std::memory_order_acquire) ? TEXT("MQTT DNS lookup was cancelled.")
				: FString::Printf(TEXT("MQTT DNS lookup failed or timed out for '%s'."), *Config.Host);
			ReleaseResolver(MoveTemp(Resolver));
			return false;
		}
		Address = Resolver->GetResolvedAddress().Clone();
		ReleaseResolver(MoveTemp(Resolver));
	}
	if (bStopRequested.load(std::memory_order_acquire) || FPlatformTime::Seconds() >= Deadline) return false;
	Address->SetPort(Config.Port);

	Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("Titan Twin MQTT"), Address->GetProtocolType());
	if (!Socket)
	{
		OutError = TEXT("Could not create an MQTT TCP socket.");
		return false;
	}
	Socket->SetNonBlocking(true);
	Socket->SetNoDelay(true);
	Socket->SetRecvErr(true);

	Socket->Connect(*Address);
	while (!bStopRequested.load(std::memory_order_acquire) && FPlatformTime::Seconds() < Deadline)
	{
		const ESocketConnectionState State = Socket->GetConnectionState();
		if (State == SCS_Connected)
		{
            if (Config.bUseTls)
            {
                Tls=MakeUnique<FTwinMqttTlsTransport>();
                return Tls->Connect(Socket,Config.Host,Config.TlsCaFile,Deadline,bStopRequested,OutError);
            }
			return true;
		}
		if (State == SCS_ConnectionError)
		{
			break;
		}
		Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(25));
	}

	OutError = bStopRequested.load(std::memory_order_acquire)
		? TEXT("MQTT connection was cancelled.")
		: FString::Printf(TEXT("Timed out connecting to MQTT broker %s:%d."), *Config.Host, Config.Port);
	return false;
}

bool FTwinMqttNativeClient::RunProtocol(FString& OutError)
{
	const FString ClientId = Config.ClientId.IsEmpty()
		? FString::Printf(TEXT("TitanTwin-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(20))
		: Config.ClientId;
	if (!SendPacket(
		FTwinMqttPacketCodec::BuildConnect(
			ClientId,
			Config.Username,
			Config.Password,
			Config.bCleanSession,
			static_cast<uint16>(FMath::Clamp(Config.KeepAliveSeconds, 0, 65535))),
		OutError))
	{
		return false;
	}

	bool bReceivedConnAck = false;
	bool bReceivedSubAck = false;
	auto WaitFor = [this, &OutError, &bReceivedConnAck, &bReceivedSubAck](bool& Flag, const TCHAR* Description)
	{
		const double Deadline = FPlatformTime::Seconds() + FMath::Max(0.1, static_cast<double>(Config.ConnectTimeoutSeconds));
		while (!Flag && !bStopRequested.load(std::memory_order_acquire) && FPlatformTime::Seconds() < Deadline)
		{
			if (!ReceiveIntoBuffer(0.05, OutError)
				|| !ProcessAvailablePackets(bReceivedConnAck, bReceivedSubAck, OutError))
			{
				return false;
			}
		}
		if (!Flag)
		{
			OutError = FString::Printf(TEXT("Timed out waiting for MQTT %s."), Description);
			return false;
		}
		return true;
	};

	if (!WaitFor(bReceivedConnAck, TEXT("CONNACK")))
	{
		return false;
	}
	TArray<uint16> ResendIds;
	PendingOutboundQos1.GenerateKeyArray(ResendIds);
	ResendIds.Sort([this](uint16 A, uint16 B) { return PendingOutboundQos1[A].PublishSequence < PendingOutboundQos1[B].PublishSequence; });
	for (uint16 Id : ResendIds)
	{
		FTwinMqttPendingQos1& Pending = PendingOutboundQos1.FindChecked(Id);
		if (!Pending.Packet.IsEmpty()) Pending.Packet[0] |= 0x08; // DUP, same packet identifier.
		if (!SendPacket(Pending.Packet, OutError)) return false;
	}
	for (auto& Pending : PendingQos2Messages)
	{
		Pending.Value.ReceivedAtSeconds = FPlatformTime::Seconds();
		if (!SendPacket(FTwinMqttPacketCodec::BuildSimplePacket(PubRecPacketType << 4, Pending.Key), OutError)) return false;
	}
	if (!SendPacket(FTwinMqttPacketCodec::BuildSubscribe(1, Config.Subscriptions), OutError)
		|| !WaitFor(bReceivedSubAck, TEXT("SUBACK")))
	{
		return false;
	}

	while (!bStopRequested.load(std::memory_order_acquire))
	{
		if (!PruneExpiredQos2Messages(FPlatformTime::Seconds(), OutError)) return false;
		if (!FlushOutgoingPublishes(OutError)
			|| !ReceiveIntoBuffer(0.05, OutError)
			|| !ProcessAvailablePackets(bReceivedConnAck, bReceivedSubAck, OutError))
		{
			return false;
		}

		const double Now = FPlatformTime::Seconds();
		const double KeepAlive = FMath::Max(1, Config.KeepAliveSeconds);
		if (bPingOutstanding && Now - PingSentSeconds > KeepAlive)
		{
			OutError = TEXT("MQTT broker did not answer PINGREQ before the keep-alive timeout.");
			return false;
		}
		if (!bPingOutstanding && Now - LastOutboundSeconds > KeepAlive * 0.5)
		{
			if (!SendPacket(FTwinMqttPacketCodec::BuildSimplePacket(PingReqPacketType << 4), OutError))
			{
				return false;
			}
			bPingOutstanding = true;
			PingSentSeconds = Now;
		}
	}

	FString IgnoredError;
	SendPacket(FTwinMqttPacketCodec::BuildSimplePacket(DisconnectPacketType << 4), IgnoredError);
	return true;
}

bool FTwinMqttNativeClient::ReceiveIntoBuffer(double WaitSeconds, FString& OutError)
{
	if (!Socket)
	{
		OutError = TEXT("MQTT socket is unavailable.");
		return false;
	}
    uint8 Temp[65536]; int32 BytesRead=0;
    if(Tls)
    {
        bool ReadWait=true; BytesRead=Tls->Read(Temp,UE_ARRAY_COUNT(Temp),ReadWait,OutError);
        if(BytesRead<0)return false;
        if(BytesRead==0)
        {
            Socket->Wait(ReadWait?ESocketWaitConditions::WaitForRead:ESocketWaitConditions::WaitForWrite,FTimespan::FromSeconds(WaitSeconds));
            return true;
        }
    }
    else
    {
        if(!Socket->Wait(ESocketWaitConditions::WaitForRead,FTimespan::FromSeconds(WaitSeconds)))
            return Socket->GetConnectionState()!=SCS_ConnectionError;
        if(!Socket->Recv(Temp,UE_ARRAY_COUNT(Temp),BytesRead))
        {
            if(Socket->GetConnectionState()==SCS_ConnectionError){OutError=TEXT("MQTT socket receive failed");return false;}
            return true;
        }
        if(BytesRead<=0){OutError=TEXT("MQTT broker closed the TCP connection");return false;}
    }
	ReceiveBuffer.Append(Temp, BytesRead);
	if (Config.RuntimeState.IsValid())
	{
		Config.RuntimeState->ReceivedBytes.fetch_add(BytesRead, std::memory_order_relaxed);
	}
	const int32 MaxBufferedBytes = Config.MaxPayloadBytes + 65536 + 5;
	if (ReceiveBuffer.Num() > MaxBufferedBytes)
	{
		OutError = FString::Printf(
			TEXT("MQTT receive buffer exceeded its %d-byte safety limit."),
			MaxBufferedBytes);
		return false;
	}
	LastInboundSeconds = FPlatformTime::Seconds();
	return true;
}

bool FTwinMqttNativeClient::SendPacket(const TArray<uint8>& Packet, FString& OutError)
{
	if (!Socket || Packet.IsEmpty())
	{
		OutError = TEXT("Cannot send an empty MQTT packet or use a closed socket.");
		return false;
	}
    if(Tls)
    {
        int32 Sent=0; const double End=FPlatformTime::Seconds()+FMath::Max(1.0,static_cast<double>(Config.ConnectTimeoutSeconds));
        while(Sent<Packet.Num()&&!bStopRequested.load(std::memory_order_acquire)&&FPlatformTime::Seconds()<End)
        {
            bool ReadWait=false; const int32 Count=Tls->Write(Packet.GetData()+Sent,Packet.Num()-Sent,ReadWait,OutError);
            if(Count<0)return false;
            if(Count==0)Socket->Wait(ReadWait?ESocketWaitConditions::WaitForRead:ESocketWaitConditions::WaitForWrite,FTimespan::FromMilliseconds(25));
            else Sent+=Count;
        }
        if(Sent!=Packet.Num()){OutError=TEXT("TLS write timed out or was cancelled");return false;}
        LastOutboundSeconds=FPlatformTime::Seconds();return true;
    }
	int32 Offset = 0;
	const double Deadline = FPlatformTime::Seconds() + 1.0;
	while (Offset < Packet.Num() && !bStopRequested.load(std::memory_order_acquire))
	{
		if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(25)))
		{
			if (FPlatformTime::Seconds() < Deadline) continue;
			OutError = TEXT("Timed out writing to the MQTT socket.");
			return false;
		}
		int32 BytesSent = 0;
		if (!Socket->Send(Packet.GetData() + Offset, Packet.Num() - Offset, BytesSent) || BytesSent <= 0)
		{
			OutError = TEXT("MQTT socket send failed.");
			return false;
		}
		Offset += BytesSent;
	}
	LastOutboundSeconds = FPlatformTime::Seconds();
	return Offset == Packet.Num();
}

bool FTwinMqttNativeClient::FlushOutgoingPublishes(FString& OutError)
{
	FPendingPublish Request;
	int32 SentThisPass = 0;
	while (SentThisPass < 128 && PendingPublishes.Dequeue(Request))
	{
		const bool bQos1 =
			Request.QualityOfService == ETwinMqttQualityOfService::AtLeastOnce;
		const uint16 PacketId = bQos1 ? AllocatePacketId() : 0;
		if (bQos1 && PacketId == 0)
		{
			OutError = TEXT("MQTT has no free packet identifier for an outbound publish.");
			if (Config.RuntimeState.IsValid())
			{
				Config.RuntimeState->PublishFailures.fetch_add(1, std::memory_order_relaxed);
			}
			return false;
		}

		const TArray<uint8> Packet = FTwinMqttPacketCodec::BuildPublish(
			Request.Topic,
			Request.Payload,
			Request.QualityOfService,
			Request.bRetain,
			PacketId);
		if (Packet.IsEmpty())
		{
			RetainedBytes.fetch_sub(Request.ReservedBytes);
			PendingPublishCount.fetch_sub(1);
			OutError = TEXT("MQTT publish could not be encoded.");
			return false;
		}
		if (bQos1)
		{
			FTwinMqttPendingQos1 Stored;
			Stored.Packet = Packet;
			Stored.ReservedBytes = Request.ReservedBytes;
			Stored.PublishSequence = Session->NextPublishSequence++;
			PendingOutboundQos1.Add(PacketId, MoveTemp(Stored));
		}
		if (!SendPacket(Packet, OutError))
		{
			if (Config.RuntimeState.IsValid())
			{
				Config.RuntimeState->PublishFailures.fetch_add(1, std::memory_order_relaxed);
			}
			return false;
		}
		if (!bQos1)
		{
			RetainedBytes.fetch_sub(Request.ReservedBytes);
			PendingPublishCount.fetch_sub(1);
		}
		if (Config.RuntimeState.IsValid())
		{
			Config.RuntimeState->PublishedMessages.fetch_add(1, std::memory_order_relaxed);
		}
		++SentThisPass;
	}
	return true;
}

uint16 FTwinMqttNativeClient::AllocatePacketId()
{
	for (int32 Attempt = 0; Attempt < MAX_uint16 - 1; ++Attempt)
	{
		const uint16 Candidate = NextPacketId;
		++NextPacketId;
		if (NextPacketId == 0 || NextPacketId == SubscribePacketId)
		{
			NextPacketId = 2;
		}
		if (Candidate != 0
			&& Candidate != SubscribePacketId
			&& !PendingOutboundQos1.Contains(Candidate))
		{
			return Candidate;
		}
	}
	return 0;
}

bool FTwinMqttNativeClient::ProcessAvailablePackets(
	bool& bOutReceivedConnAck,
	bool& bOutReceivedSubAck,
	FString& OutError)
{
	int32 ProcessedBytes = 0;
	while (ProcessedBytes < ReceiveBuffer.Num())
	{
		FTwinMqttDecodedPacket Packet;
		int32 ConsumedBytes = 0;
		const TArrayView<const uint8> Unprocessed(
			ReceiveBuffer.GetData() + ProcessedBytes,
			ReceiveBuffer.Num() - ProcessedBytes);
		const ETwinMqttDecodeResult Result = FTwinMqttPacketCodec::Decode(
			Unprocessed,
			Config.MaxPayloadBytes + 65536,
			Packet,
			ConsumedBytes,
			OutError);
		if (Result == ETwinMqttDecodeResult::NeedMoreData)
		{
			if (ProcessedBytes > 0)
			{
				ReceiveBuffer.RemoveAt(0, ProcessedBytes, EAllowShrinking::No);
			}
			return true;
		}
		if (Result == ETwinMqttDecodeResult::Malformed)
		{
			RecordProtocolError();
			return false;
		}
		ProcessedBytes += ConsumedBytes;
		if (!HandlePacket(Packet, bOutReceivedConnAck, bOutReceivedSubAck, OutError))
		{
			return false;
		}
	}
	ReceiveBuffer.Reset();
	return true;
}

bool FTwinMqttNativeClient::HandlePacket(
	const FTwinMqttDecodedPacket& Packet,
	bool& bOutReceivedConnAck,
	bool& bOutReceivedSubAck,
	FString& OutError)
{
	auto FailProtocol = [this, &OutError](const FString& Message)
	{
		OutError = Message;
		RecordProtocolError();
		return false;
	};

	switch (Packet.Type)
	{
	case ConnAckPacketType:
		if (Packet.Body.Num() != 2 || (Packet.Body[0] & 0xfe) != 0)
		{
			return FailProtocol(TEXT("MQTT CONNACK packet is malformed."));
		}
		if (bOutReceivedConnAck)
		{
			return FailProtocol(TEXT("MQTT broker sent a duplicate CONNACK packet."));
		}
		if ((Packet.Body[0] & 0x01) != 0
			&& (Config.bCleanSession || Packet.Body[1] != 0))
		{
			return FailProtocol(TEXT("MQTT broker returned Session Present for a clean session."));
		}
		if (Packet.Body[1] > 5)
		{
			return FailProtocol(TEXT("MQTT CONNACK packet contains an invalid return code."));
		}
		if (Packet.Body[1] != 0)
		{
			OutError = FString::Printf(
				TEXT("MQTT broker rejected CONNECT with return code %d."),
				Packet.Body[1]);
			return false;
		}
		bOutReceivedConnAck = true;
		if (!Config.bCleanSession && (Packet.Body[0] & 0x01) == 0)
		{
			for (const auto& Pending : PendingQos2Messages) RetainedBytes.fetch_sub(Pending.Value.ReservedBytes);
			PendingQos2Messages.Reset();
		}
		return true;

	case SubAckPacketType:
		if (Packet.Body.Num() < 3
			|| Packet.Body.Num() - 2 != Config.Subscriptions.Num()
			|| bOutReceivedSubAck)
		{
			return FailProtocol(TEXT("MQTT SUBACK packet is malformed or duplicated."));
		}
		{
			int32 Offset = 0;
			uint16 PacketId = 0;
			if (!ReadUInt16(Packet.Body, Offset, PacketId) || PacketId != SubscribePacketId)
			{
				return FailProtocol(TEXT("MQTT SUBACK packet identifier does not match SUBSCRIBE."));
			}
		}
		FailedSubscriptionCount = 0;
		for (int32 Index = 2; Index < Packet.Body.Num(); ++Index)
		{
			const uint8 ResultCode = Packet.Body[Index];
			if (ResultCode == 0x80)
			{
				++FailedSubscriptionCount;
			}
			else if (ResultCode > 2)
			{
				return FailProtocol(TEXT("MQTT SUBACK contains an invalid granted QoS value."));
			}
		}
		bOutReceivedSubAck = true;
		bConnected.store(true, std::memory_order_release);
		bReadyEventQueued = true;
		{
			FTwinMqttNativeEvent Ready;
			Ready.Type = ETwinMqttNativeEventType::Ready;
			Ready.FailedSubscriptionCount = FailedSubscriptionCount;
			EnqueueControlEvent(MoveTemp(Ready));
			for (FTwinMqttNativeEvent& Pending : PreReadyMessages) Events.Enqueue(MoveTemp(Pending));
			PreReadyMessages.Reset();
		}
		return true;

	case PublishPacketType:
		HandlePublish(Packet, OutError);
		return OutError.IsEmpty();

	case PubAckPacketType:
	{
		int32 Offset = 0;
		uint16 PacketId = 0;
		if (Packet.Body.Num() != 2
			|| !ReadUInt16(Packet.Body, Offset, PacketId)
			|| PacketId == 0)
		{
			return FailProtocol(TEXT("MQTT PUBACK packet has an invalid packet identifier."));
		}
		if (FTwinMqttPendingQos1* Pending = PendingOutboundQos1.Find(PacketId))
		{
			RetainedBytes.fetch_sub(Pending->ReservedBytes);
			PendingPublishCount.fetch_sub(1);
			PendingOutboundQos1.Remove(PacketId);
		}
		return true;
	}

	case PubRelPacketType:
	{
		int32 Offset = 0;
		uint16 PacketId = 0;
		if (Packet.Body.Num() != 2 || !ReadUInt16(Packet.Body, Offset, PacketId) || PacketId == 0)
		{
			return FailProtocol(TEXT("MQTT PUBREL packet has an invalid packet identifier."));
		}
		if (FPendingQos2Message* Pending = PendingQos2Messages.Find(PacketId))
		{
			if (!DeliverMessage(Pending->Packet))
			{
				OutError = TEXT("MQTT application queue is full; QoS 2 ownership was retained for reconnect.");
				return false;
			}
			RetainedBytes.fetch_sub(Pending->ReservedBytes);
			PendingQos2Messages.Remove(PacketId);
		}
		return SendPacket(
			FTwinMqttPacketCodec::BuildSimplePacket(static_cast<uint8>((PubCompPacketType << 4)), PacketId),
			OutError);
	}

	case PingRespPacketType:
		if (!Packet.Body.IsEmpty())
		{
			return FailProtocol(TEXT("MQTT PINGRESP packet must have an empty body."));
		}
		bPingOutstanding = false;
		return true;

	case DisconnectPacketType:
		OutError = TEXT("MQTT broker sent DISCONNECT.");
		return false;

	default:
		return FailProtocol(FString::Printf(
			TEXT("MQTT broker sent unexpected packet type %u."),
			Packet.Type));
	}
}

void FTwinMqttNativeClient::HandlePublish(const FTwinMqttDecodedPacket& Packet, FString& OutError)
{
	if (Packet.PayloadByteCount > Config.MaxPayloadBytes)
	{
		OutError = TEXT("MQTT payload exceeds the configured safety limit.");
		if (Config.RuntimeState.IsValid())
		{
			Config.RuntimeState->OversizedMessages.fetch_add(1, std::memory_order_relaxed);
		}
		return;
	}

	if (Packet.Qos == 0)
	{
		DeliverMessage(Packet);
		return;
	}
	if (Packet.Qos == 1)
	{
		if (!DeliverMessage(Packet))
		{
			OutError = TEXT("MQTT application queue is full; QoS 1 was not acknowledged.");
			return;
		}
		SendPacket(
			FTwinMqttPacketCodec::BuildSimplePacket(PubAckPacketType << 4, Packet.PacketId),
			OutError);
		return;
	}

	if (!PendingQos2Messages.Contains(Packet.PacketId))
	{
		if (PendingQos2Messages.Num() >= Config.MaxPendingQos2Messages)
		{
			OutError = TEXT("MQTT pending QoS 2 message limit was reached; closing the connection to protect memory.");
			if (Config.RuntimeState.IsValid())
			{
				Config.RuntimeState->DroppedNativeMessages.fetch_add(1, std::memory_order_relaxed);
			}
			return;
		}
		FPendingQos2Message Pending;
		Pending.ReservedBytes = sizeof(FPendingQos2Message) + Packet.Body.GetAllocatedSize()
			+ Packet.Topic.GetAllocatedSize() + Packet.Payload.GetAllocatedSize();
		if (!ReserveBytes(Pending.ReservedBytes))
		{
			OutError = TEXT("MQTT retained byte budget is full; closing without accepting new QoS 2 ownership.");
			return;
		}
		Pending.Packet = Packet;
		Pending.ReceivedAtSeconds = FPlatformTime::Seconds();
		PendingQos2Messages.Add(Packet.PacketId, MoveTemp(Pending));
	}
	SendPacket(
		FTwinMqttPacketCodec::BuildSimplePacket(PubRecPacketType << 4, Packet.PacketId),
		OutError);
}

bool FTwinMqttNativeClient::DeliverMessage(const FTwinMqttDecodedPacket& Packet)
{
	const FTwinMqttSubscriptionDefinition* Definition = FindDefinitionForTopic(Packet.Topic);
	if (!Definition)
	{
		return true;
	}
	if (!Packet.bPayloadIsText)
	{
		// Report it as handled so QoS 1/2 messages are acknowledged and not redelivered forever.
		if (Config.RuntimeState.IsValid())
		{
			Config.RuntimeState->ParseFailures.fetch_add(1, std::memory_order_relaxed);
		}
		return true;
	}
	FTwinMqttNativeEvent Event;
	Event.Type = ETwinMqttNativeEventType::Message;
	Event.Topic = Packet.Topic;
	Event.Payload = Packet.Payload;
	Event.Timestamp = FDateTime::UtcNow();
	Event.Definition = *Definition;
	return EnqueueMessageEvent(MoveTemp(Event));
}

bool FTwinMqttNativeClient::EnqueueMessageEvent(FTwinMqttNativeEvent&& Event)
{
	int32 PendingCount = PendingMessageEventCount.load(std::memory_order_acquire);
	while (PendingCount < Config.MaxPendingMessageEvents)
	{
		if (PendingMessageEventCount.compare_exchange_weak(
			PendingCount,
			PendingCount + 1,
			std::memory_order_acq_rel,
			std::memory_order_acquire))
		{
			Event.ReservedBytes = sizeof(FTwinMqttNativeEvent) + Event.Topic.GetAllocatedSize()
				+ Event.Payload.GetAllocatedSize() + Event.Definition.TopicFilter.GetAllocatedSize()
				+ Event.Definition.JsonValueField.GetAllocatedSize() + Event.Definition.JsonUnitField.GetAllocatedSize()
				+ Event.Definition.JsonTimestampField.GetAllocatedSize() + Event.Definition.JsonQualityField.GetAllocatedSize();
			if (!ReserveBytes(Event.ReservedBytes))
			{
				PendingMessageEventCount.fetch_sub(1);
				if (Config.RuntimeState.IsValid()) Config.RuntimeState->DroppedNativeMessages.fetch_add(1);
				return false;
			}
			if (!bReadyEventQueued) PreReadyMessages.Add(MoveTemp(Event));
			else Events.Enqueue(MoveTemp(Event));
			return true;
		}
	}

	if (Config.RuntimeState.IsValid())
	{
		Config.RuntimeState->DroppedNativeMessages.fetch_add(1, std::memory_order_relaxed);
	}
	return false;
}

void FTwinMqttNativeClient::EnqueueControlEvent(FTwinMqttNativeEvent&& Event)
{
	Events.Enqueue(MoveTemp(Event));
}

void FTwinMqttNativeClient::RecordProtocolError()
{
	if (Config.RuntimeState.IsValid())
	{
		Config.RuntimeState->ProtocolErrors.fetch_add(1, std::memory_order_relaxed);
	}
}

bool FTwinMqttNativeClient::ReserveBytes(int64 Bytes)
{
	if (Bytes < 0 || Bytes > Config.MaxRetainedBytes) return false;
	int64 Current = RetainedBytes.load(std::memory_order_acquire);
	while (Current <= Config.MaxRetainedBytes - Bytes)
		if (RetainedBytes.compare_exchange_weak(Current, Current + Bytes, std::memory_order_acq_rel)) return true;
	return false;
}

bool FTwinMqttNativeClient::PruneExpiredQos2Messages(double NowSeconds, FString& OutError)
{
	for (const auto& Pending : PendingQos2Messages)
	{
		if (NowSeconds - Pending.Value.ReceivedAtSeconds >= Config.Qos2MessageTimeoutSeconds)
		{
			// Never forget accepted QoS 2 payloads and then answer PUBREL as if delivered.
			OutError = TEXT("MQTT QoS 2 exchange timed out; state is retained while reconnecting.");
			return false;
		}
	}
	return true;
}

const FTwinMqttSubscriptionDefinition* FTwinMqttNativeClient::FindDefinitionForTopic(const FString& Topic) const
{
	const FTwinMqttSubscriptionDefinition* Best = nullptr;
	int32 BestScore = MIN_int32;
	for (const FTwinMqttSubscriptionDefinition& Definition : Config.Subscriptions)
	{
		if (!FTwinMqttPacketCodec::TopicMatchesFilter(Topic, Definition.TopicFilter))
		{
			continue;
		}
		const int32 Score = Definition.TopicFilter.Len()
			- CountCharacter(Definition.TopicFilter, TEXT('+')) * 8
			- CountCharacter(Definition.TopicFilter, TEXT('#')) * 16;
		if (!Best || Score > BestScore)
		{
			Best = &Definition;
			BestScore = Score;
		}
	}
	return Best;
}

void FTwinMqttNativeClient::CloseSocket()
{
    Tls.Reset();
	if (!Socket)
	{
		return;
	}
	Socket->Close();
	if (ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
}

void FTwinMqttNativeClient::EnqueueStopped(const FString& Error)
{
	if (bStoppedEventSent)
	{
		return;
	}
	bStoppedEventSent = true;
	FTwinMqttNativeEvent Event;
	Event.Type = ETwinMqttNativeEventType::Stopped;
	Event.Error = Error;
	EnqueueControlEvent(MoveTemp(Event));
}
