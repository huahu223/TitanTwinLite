// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinMqttSubsystem.h"

#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "TitanTwinMqtt.h"
#include "TimerManager.h"
#include "TwinReplaySafety.h"
#include "TwinDataSourceSubsystem.h"
#include "TwinMqttNativeClient.h"
#include "TwinMqttPayloadParser.h"
#include "TwinMqttRuntimeState.h"
#include "TwinMqttSettings.h"

namespace
{
	void ProcessIncomingMessage(
		const TSharedPtr<FTwinMqttRuntimeState, ESPMode::ThreadSafe>& State,
		uint64 Generation,
		const FTwinMqttSubscriptionDefinition& Definition,
		const FString& Topic,
		const FString& Payload,
		const FDateTime& Timestamp)
	{
		if (!State.IsValid()
			|| State->ActiveGeneration.load(std::memory_order_acquire) != Generation
			|| !State->bAccepting.load(std::memory_order_acquire))
		{
			return;
		}

		State->ReceivedMessages.fetch_add(1, std::memory_order_relaxed);
		const FTCHARToUTF8 PayloadUtf8(*Payload);
		if (PayloadUtf8.Length() > State->MaxPayloadBytes)
		{
			State->OversizedMessages.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		TArray<FTwinDataPoint> Points;
		FString ParseError;
		if (!FTwinMqttPayloadParser::ParseMany(
			Definition,
			State->SourceId,
			Topic,
			Payload,
			Timestamp,
			Points,
			ParseError))
		{
			State->ParseFailures.fetch_add(1, std::memory_order_relaxed);
			UE_LOG(
				LogTitanTwinMqtt,
				Warning,
				TEXT("Rejected MQTT payload on '%s': %s"),
				*Topic,
				*ParseError);
			return;
		}

        State->ParsedPoints.fetch_add(Points.Num(), std::memory_order_relaxed);
        for (const auto& Point:Points)
            if (!State->Ingress.IsValid() || !State->Ingress->Enqueue(Point))
                State->QueueRejectedMessages.fetch_add(1,std::memory_order_relaxed);
	}
}

bool UTwinMqttSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UTwinMqttSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UTwinDataSourceSubsystem>();

	RuntimeState = MakeShared<FTwinMqttRuntimeState, ESPMode::ThreadSafe>();
	if (UTwinDataSourceSubsystem* DataSource = GetWorld()->GetSubsystem<UTwinDataSourceSubsystem>())
	{
		RuntimeState->Ingress = DataSource->GetIngress();
	}

	const UTwinMqttSettings* Settings = GetDefault<UTwinMqttSettings>();
	ActiveSourceId = Settings ? Settings->SourceId : FName(TEXT("Mqtt"));
	RuntimeState->SourceId = ActiveSourceId;
	RuntimeState->MaxPayloadBytes = Settings
		? FMath::Clamp(Settings->MaxPayloadBytes, 1, 16 * 1024 * 1024)
		: 1048576;
	NativePumpTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UTwinMqttSubsystem::PumpNativeEvents),
		0.01f);
	ReportSourceState(ETwinDataSourceState::Stopped);
}

void UTwinMqttSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (const UTwinMqttSettings* Settings = GetDefault<UTwinMqttSettings>();
		Settings && Settings->bAutoConnect)
	{
		Connect();
	}
}

void UTwinMqttSubsystem::Deinitialize()
{
	bManualDisconnect = true;
	ClearReconnectTimer();
	InvalidateConnection();
	ReleaseClient();
	if (NativePumpTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(NativePumpTickerHandle);
		NativePumpTickerHandle.Reset();
	}
	ReportSourceState(ETwinDataSourceState::Stopped);
	RuntimeState.Reset();
	Super::Deinitialize();
}

bool UTwinMqttSubsystem::Connect()
{
	check(IsInGameThread());
	if (bConnected || bConnecting)
	{
		return false;
	}

	FString ValidationError;
	if (!ValidateSettings(ValidationError))
	{
		LastError = ValidationError;
		ReportSourceState(ETwinDataSourceState::Failed, LastError);
		return false;
	}

	const UTwinMqttSettings* Settings = GetDefault<UTwinMqttSettings>();
	ActiveSourceId = Settings->SourceId;
	bManualDisconnect = false;
	bConnecting = true;
	bConnected = false;
	LastError.Reset();
	ClearReconnectTimer();
	const uint64 Generation = InvalidateConnection();
	ReleaseClient();

	if (!RuntimeState.IsValid())
	{
		RuntimeState = MakeShared<FTwinMqttRuntimeState, ESPMode::ThreadSafe>();
	}
	if (!RuntimeState->Ingress.IsValid())
	{
		if (UTwinDataSourceSubsystem* DataSource = GetWorld()->GetSubsystem<UTwinDataSourceSubsystem>())
		{
			RuntimeState->Ingress = DataSource->GetIngress();
		}
	}
	RuntimeState->SourceId = ActiveSourceId;
	RuntimeState->MaxPayloadBytes = FMath::Clamp(Settings->MaxPayloadBytes, 1, 16 * 1024 * 1024);
	RuntimeState->ActiveGeneration.store(Generation, std::memory_order_release);
	RuntimeState->bAccepting.store(false, std::memory_order_release);
	RuntimeState->ConnectAttempts.fetch_add(1, std::memory_order_relaxed);

	FTwinMqttNativeConfig Config;
	Config.Host = Settings->Host.TrimStartAndEnd();
	Config.Port = Settings->Port;
	Config.ClientId = Settings->ClientId.TrimStartAndEnd();
	Config.Username = Settings->UsernameEnvironmentVariable.IsEmpty()
		? FString()
		: FPlatformMisc::GetEnvironmentVariable(*Settings->UsernameEnvironmentVariable);
	Config.Password = Settings->PasswordEnvironmentVariable.IsEmpty()
		? FString()
		: FPlatformMisc::GetEnvironmentVariable(*Settings->PasswordEnvironmentVariable);
	Config.bCleanSession = Settings->bCleanSession;
	Config.bUseTls = Settings->bUseTls;
    Config.TlsCaFile=Settings->TlsCaFile;
	Config.KeepAliveSeconds = Settings->KeepAliveSeconds;
	Config.ConnectTimeoutSeconds = Settings->ConnectTimeoutSeconds;
	Config.MaxPayloadBytes = Settings->MaxPayloadBytes;
	Config.MaxPendingMessageEvents = Settings->MaxPendingNativeMessages;
	Config.MaxPendingPublishes = Settings->MaxPendingPublishes;
	Config.MaxRetainedBytes = Settings->MaxRetainedNativeBytes;
	Config.MaxPendingQos2Messages = Settings->MaxPendingQos2Messages;
	Config.Qos2MessageTimeoutSeconds = Settings->Qos2MessageTimeoutSeconds;
	Config.RuntimeState = RuntimeState;
	for (const FTwinMqttSubscriptionDefinition& Definition : Settings->Subscriptions)
	{
		if (Definition.bEnabled)
		{
			Config.Subscriptions.Add(Definition);
		}
	}
	if (!Config.Validate(LastError))
	{
		bConnecting = false;
		ReportSourceState(ETwinDataSourceState::Failed, LastError);
		return false;
	}

	const FString Identity = FString::Printf(TEXT("%d:%s|%d|%d:%s|%d:%s"),
		Config.Host.Len(), *Config.Host, Config.Port, Config.ClientId.Len(), *Config.ClientId,
		Config.Username.Len(), *Config.Username);
	const bool bNewSession = Config.bCleanSession || !PersistentSession.IsValid() || PersistentSessionIdentity != Identity;
	if (bNewSession)
	{
		PersistentSession = MakeShared<FTwinMqttPersistentSession, ESPMode::ThreadSafe>();
		PersistentSessionIdentity = Identity;
	}
	Config.PersistentSession = PersistentSession;

	Client = MakeShared<FTwinMqttNativeClient, ESPMode::ThreadSafe>(MoveTemp(Config));
	if (!Client->Start(LastError))
	{
		bConnecting = false;
		ReleaseClient();
		ReportSourceState(ETwinDataSourceState::Failed, LastError);
		return false;
	}

	ReportSourceState(ETwinDataSourceState::Starting);
	return true;
}

void UTwinMqttSubsystem::Disconnect()
{
	check(IsInGameThread());
	bManualDisconnect = true;
	ClearReconnectTimer();
	InvalidateConnection();
	const bool bWasConnected = bConnected;
	ReleaseClient();
	bConnected = false;
	bConnecting = false;
	ReconnectAttempt = 0;
	LastError.Reset();
	ReportSourceState(ETwinDataSourceState::Stopped);
	if (bWasConnected)
	{
		OnConnectionChanged.Broadcast(false);
	}
}

bool UTwinMqttSubsystem::Reconnect()
{
	Disconnect();
	bManualDisconnect = false;
	return Connect();
}

bool UTwinMqttSubsystem::IsConnected() const
{
	return bConnected && Client.IsValid() && Client->IsConnected();
}

bool UTwinMqttSubsystem::IsConnecting() const
{
	return bConnecting;
}

FString UTwinMqttSubsystem::GetLastError() const
{
	return LastError;
}

FTwinMqttMetrics UTwinMqttSubsystem::GetMetrics() const
{
	FTwinMqttMetrics Metrics;
	if (!RuntimeState.IsValid())
	{
		return Metrics;
	}
	Metrics.ConnectAttempts = RuntimeState->ConnectAttempts.load(std::memory_order_relaxed);
	Metrics.SuccessfulConnections = RuntimeState->SuccessfulConnections.load(std::memory_order_relaxed);
	Metrics.Reconnects = RuntimeState->Reconnects.load(std::memory_order_relaxed);
	Metrics.ReceivedMessages = RuntimeState->ReceivedMessages.load(std::memory_order_relaxed);
	Metrics.ParsedPoints = RuntimeState->ParsedPoints.load(std::memory_order_relaxed);
	Metrics.ParseFailures = RuntimeState->ParseFailures.load(std::memory_order_relaxed);
	Metrics.OversizedMessages = RuntimeState->OversizedMessages.load(std::memory_order_relaxed);
	Metrics.QueueRejectedMessages = RuntimeState->QueueRejectedMessages.load(std::memory_order_relaxed);
	Metrics.DroppedNativeMessages = RuntimeState->DroppedNativeMessages.load(std::memory_order_relaxed);
	Metrics.ProtocolErrors = RuntimeState->ProtocolErrors.load(std::memory_order_relaxed);
	Metrics.ReceivedBytes = RuntimeState->ReceivedBytes.load(std::memory_order_relaxed);
	Metrics.PublishedMessages = RuntimeState->PublishedMessages.load(std::memory_order_relaxed);
	Metrics.PublishFailures = RuntimeState->PublishFailures.load(std::memory_order_relaxed);
	return Metrics;
}

bool UTwinMqttSubsystem::Publish(
	const FString& Topic,
	const FString& Payload,
	ETwinMqttQualityOfService QualityOfService,
	bool bRetain)
{
	check(IsInGameThread());
	if (!Client.IsValid())
	{
		LastError = TEXT("MQTT client is not connected.");
		if (RuntimeState.IsValid())
		{
			RuntimeState->PublishFailures.fetch_add(1, std::memory_order_relaxed);
		}
		return false;
	}
	if (!Client->EnqueuePublish(Topic, Payload, QualityOfService, bRetain, LastError))
	{
		if (RuntimeState.IsValid())
		{
			RuntimeState->PublishFailures.fetch_add(1, std::memory_order_relaxed);
		}
		return false;
	}
	return true;
}

bool UTwinMqttSubsystem::PumpNativeEvents(float DeltaSeconds)
{
	if (!Client.IsValid())
	{
		return true;
	}

	FTwinMqttNativeEvent Event;
	int32 Processed = 0;
	const UTwinMqttSettings* Settings = GetDefault<UTwinMqttSettings>();
	const int32 MaxEvents = Settings
		? FMath::Clamp(Settings->MaxNativeMessagesPerPump, 1, 8192)
		: 512;
	while (Processed < MaxEvents && Client.IsValid() && Client->DequeueEvent(Event))
	{
		HandleNativeEvent(MoveTemp(Event));
		++Processed;
	}
	return true;
}

void UTwinMqttSubsystem::HandleNativeEvent(FTwinMqttNativeEvent&& Event)
{
	check(IsInGameThread());
	switch (Event.Type)
	{
	case ETwinMqttNativeEventType::Ready:
	{
		const uint64 ReadyGeneration = ConnectionGeneration;
		bConnecting = false;
		bConnected = true;
		RuntimeState->bAccepting.store(true, std::memory_order_release);
		RuntimeState->SuccessfulConnections.fetch_add(1, std::memory_order_relaxed);
		if (ReconnectAttempt > 0)
		{
			RuntimeState->Reconnects.fetch_add(1, std::memory_order_relaxed);
		}
		ReconnectAttempt = 0;
		LastError.Reset();
		OnConnectionChanged.Broadcast(true);
		if (ReadyGeneration != ConnectionGeneration || !bConnected) break;
		if (Event.FailedSubscriptionCount > 0)
		{
			LastError = FString::Printf(
				TEXT("Connected, but %d MQTT subscription(s) failed."),
				Event.FailedSubscriptionCount);
			ReportSourceState(ETwinDataSourceState::Degraded, LastError);
		}
		else
		{
			ReportSourceState(ETwinDataSourceState::Running);
		}
		break;
	}

	case ETwinMqttNativeEventType::Message:
		ProcessIncomingMessage(
			RuntimeState,
			ConnectionGeneration,
			Event.Definition,
			Event.Topic,
			Event.Payload,
			Event.Timestamp);
		break;

	case ETwinMqttNativeEventType::Stopped:
	{
		if (bManualDisconnect)
		{
			break;
		}
		const bool bWasConnected = bConnected;
		bConnected = false;
		bConnecting = false;
		InvalidateConnection();
		ReleaseClient();
		const uint64 StoppedGeneration = ConnectionGeneration;
		if (bWasConnected)
		{
			OnConnectionChanged.Broadcast(false);
		}
		if (StoppedGeneration != ConnectionGeneration) break;
		ScheduleReconnect(Event.Error.IsEmpty()
			? TEXT("MQTT connection closed unexpectedly.")
			: Event.Error);
		break;
	}
	}
}

void UTwinMqttSubsystem::HandleReconnectTimer()
{
	Connect();
}

void UTwinMqttSubsystem::ScheduleReconnect(const FString& Reason)
{
	check(IsInGameThread());
	LastError = Reason;
	const UTwinMqttSettings* Settings = GetDefault<UTwinMqttSettings>();
	if (bManualDisconnect || !Settings || !Settings->bAutoReconnect || !GetWorld())
	{
		ReportSourceState(ETwinDataSourceState::Failed, LastError);
		return;
	}

	const int32 SafeAttempt = FMath::Min(ReconnectAttempt, 20);
	const float ExponentialDelay = Settings->ReconnectInitialDelaySeconds
		* FMath::Pow(2.0f, static_cast<float>(SafeAttempt));
	const float Delay = FMath::Min(Settings->ReconnectMaxDelaySeconds, ExponentialDelay)
		* FMath::FRandRange(0.85f, 1.15f);
	++ReconnectAttempt;
	ReportSourceState(ETwinDataSourceState::Reconnecting, LastError);
	GetWorld()->GetTimerManager().SetTimer(
		ReconnectTimerHandle,
		this,
		&UTwinMqttSubsystem::HandleReconnectTimer,
		FMath::Max(0.1f, Delay),
		false);
}

void UTwinMqttSubsystem::ClearReconnectTimer()
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(ReconnectTimerHandle);
	}
}

uint64 UTwinMqttSubsystem::InvalidateConnection()
{
	++ConnectionGeneration;
	if (RuntimeState.IsValid())
	{
		RuntimeState->bAccepting.store(false, std::memory_order_release);
		RuntimeState->ActiveGeneration.store(ConnectionGeneration, std::memory_order_release);
	}
	return ConnectionGeneration;
}

void UTwinMqttSubsystem::ReleaseClient()
{
	if (Client.IsValid())
	{
		Client->StopAndWait();
		Client.Reset();
	}
}

void UTwinMqttSubsystem::ReportSourceState(
	ETwinDataSourceState State,
	const FString& ErrorMessage) const
{
	if (GetWorld())
	{
		if (UTwinDataSourceSubsystem* DataSource = GetWorld()->GetSubsystem<UTwinDataSourceSubsystem>())
		{
			DataSource->ReportSourceState(ActiveSourceId, State, ErrorMessage);
		}
	}
}

bool UTwinMqttSubsystem::ValidateSettings(FString& OutError) const
{
	OutError.Reset();
	const UTwinMqttSettings* Settings = GetDefault<UTwinMqttSettings>();
	if (!Settings)
	{
		OutError = TEXT("Titan Twin MQTT settings are unavailable.");
		return false;
	}
	if (Settings->SourceId.IsNone())
	{
		OutError = TEXT("Source ID is required.");
		return false;
	}
	if (Settings->Host.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("MQTT broker host is required.");
		return false;
	}
	if (Settings->Port < 1 || Settings->Port > 65535)
	{
		OutError = TEXT("MQTT broker port must be between 1 and 65535.");
		return false;
	}
	if (!Settings->bCleanSession && Settings->ClientId.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("A stable MQTT Client ID is required when Clean Session is disabled.");
		return false;
	}
	if (Settings->KeepAliveSeconds < 1 || Settings->KeepAliveSeconds > 65535)
	{
		OutError = TEXT("MQTT keep-alive must be between 1 and 65535 seconds.");
		return false;
	}
	if (!FMath::IsFinite(Settings->ConnectTimeoutSeconds)
		|| Settings->ConnectTimeoutSeconds < 0.1f
		|| Settings->ConnectTimeoutSeconds > 300.0f)
	{
		OutError = TEXT("MQTT connect timeout must be between 0.1 and 300 seconds.");
		return false;
	}
	if (!FMath::IsFinite(Settings->ReconnectInitialDelaySeconds)
		|| !FMath::IsFinite(Settings->ReconnectMaxDelaySeconds)
		|| Settings->ReconnectInitialDelaySeconds < 0.1f
		|| Settings->ReconnectMaxDelaySeconds < 1.0f
		|| Settings->ReconnectInitialDelaySeconds > Settings->ReconnectMaxDelaySeconds)
	{
		OutError = TEXT("MQTT reconnect delays are invalid or the initial delay exceeds the maximum delay.");
		return false;
	}
	if (Settings->MaxPayloadBytes < 1 || Settings->MaxPayloadBytes > 16 * 1024 * 1024)
	{
		OutError = TEXT("MQTT payload limit must be between 1 byte and 16 MiB.");
		return false;
	}
	if (Settings->MaxPendingNativeMessages < 16 || Settings->MaxPendingNativeMessages > 65536
		|| Settings->MaxRetainedNativeBytes < 65536 || Settings->MaxRetainedNativeBytes > 268435456
		|| Settings->MaxPendingPublishes < 16 || Settings->MaxPendingPublishes > 65536
		|| Settings->MaxNativeMessagesPerPump < 1 || Settings->MaxNativeMessagesPerPump > 8192
		|| Settings->MaxPendingQos2Messages < 1 || Settings->MaxPendingQos2Messages > 65535
		|| !FMath::IsFinite(Settings->Qos2MessageTimeoutSeconds)
		|| Settings->Qos2MessageTimeoutSeconds < 1.0f
		|| Settings->Qos2MessageTimeoutSeconds > 3600.0f)
	{
		OutError = TEXT("One or more MQTT safety limits are outside their supported ranges.");
		return false;
	}
	int32 EnabledCount = 0;
	TSet<FString> UniqueFilters;
	for (const FTwinMqttSubscriptionDefinition& Definition : Settings->Subscriptions)
	{
		if (!Definition.bEnabled)
		{
			continue;
		}
		++EnabledCount;
		FString DefinitionError;
		if (!Definition.IsValid(&DefinitionError))
		{
			OutError = DefinitionError;
			return false;
		}
		if (!FTwinMqttPacketCodec::IsValidTopicFilter(Definition.TopicFilter, &OutError))
		{
			return false;
		}
		if (UniqueFilters.Contains(Definition.TopicFilter))
		{
			OutError = FString::Printf(
				TEXT("Duplicate MQTT topic filter: %s"),
				*Definition.TopicFilter);
			return false;
		}
		UniqueFilters.Add(Definition.TopicFilter);
	}
	if (EnabledCount == 0)
	{
		OutError = TEXT("At least one enabled MQTT subscription is required.");
		return false;
	}
	if (EnabledCount > 1024)
	{
		OutError = TEXT("At most 1024 enabled MQTT subscriptions are supported per connection.");
		return false;
	}
	return true;
}
