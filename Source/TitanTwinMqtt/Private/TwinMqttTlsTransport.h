// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once
#include "CoreMinimal.h"
#include <atomic>
class FSocket;
struct ssl_st;
struct ssl_ctx_st;
struct bio_method_st;

/** One worker owns all TLS state. No verification bypass or plaintext fallback. */
class FTwinMqttTlsTransport final
{
public:
    ~FTwinMqttTlsTransport();
    bool Connect(FSocket* Socket, const FString& Host, const FString& CaFile, double Deadline,
        const std::atomic<bool>& Stop, FString& Error);
    // >0 bytes, 0 would-block, -1 closed/fatal. WaitForRead tells the caller how to wait.
    int32 Read(uint8* Data, int32 Size, bool& WaitForRead, FString& Error);
    int32 Write(const uint8* Data, int32 Size, bool& WaitForRead, FString& Error);
    bool HasPending() const;
private:
    int32 Result(int32 Count, bool& WaitForRead, FString& Error);
    ssl_st* Session = nullptr;
    ssl_ctx_st* Context = nullptr;
    bio_method_st* Method = nullptr;
    FString ServerHost;
};
