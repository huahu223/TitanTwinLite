// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"

namespace TitanTwin::TransportSecurity
{
	inline bool TryGetOrigin(const FString& Url, FString& OutOrigin, bool& bOutLoopback)
	{
		OutOrigin.Reset();
		bOutLoopback = false;
		FString Scheme, Remainder;
		if (!Url.Split(TEXT("://"), &Scheme, &Remainder)) return false;
		Scheme.ToLowerInline();
		const bool bSecure = Scheme == TEXT("https") || Scheme == TEXT("wss");
		if (!bSecure && Scheme != TEXT("http") && Scheme != TEXT("ws")) return false;
		int32 End = Remainder.Len();
		for (int32 Index = 0; Index < Remainder.Len(); ++Index)
			if (Remainder[Index] == TEXT('/') || Remainder[Index] == TEXT('?') || Remainder[Index] == TEXT('#'))
			{ End = Index; break; }
		FString Authority = Remainder.Left(End);
		if (Authority.IsEmpty() || Authority.Contains(TEXT("@")) || Authority.Contains(TEXT("\\"))) return false;
		FString Host, PortText;
		if (Authority.StartsWith(TEXT("[")))
		{
			int32 Closing = INDEX_NONE;
			if (!Authority.FindChar(TEXT(']'), Closing) || Closing < 2) return false;
			Host = Authority.Mid(1, Closing - 1);
			if (Closing + 1 < Authority.Len())
			{
				if (Authority[Closing + 1] != TEXT(':')) return false;
				PortText = Authority.Mid(Closing + 2);
				if (PortText.IsEmpty()) return false;
			}
		}
		else
		{
			if (!Authority.Split(TEXT(":"), &Host, &PortText)) Host = Authority;
			else if (PortText.IsEmpty() || PortText.Contains(TEXT(":"))) return false;
		}
		Host.ToLowerInline();
		Host.RemoveFromEnd(TEXT("."));
		if (Host.IsEmpty()) return false;
		for (TCHAR C : Host)
			if (!(FChar::IsAlnum(C) && C < 128) && C != TEXT('.') && C != TEXT('-') && C != TEXT(':')) return false;
		int32 Port = bSecure ? 443 : 80;
		if (!PortText.IsEmpty())
		{
			for (TCHAR C : PortText) if (C < TEXT('0') || C > TEXT('9')) return false;
			if (!LexTryParseString(Port, *PortText) || Port < 1 || Port > 65535) return false;
		}
		bOutLoopback = Host == TEXT("localhost") || Host == TEXT("127.0.0.1") || Host == TEXT("::1");
		const FString DisplayHost = Host.Contains(TEXT(":")) ? FString::Printf(TEXT("[%s]"), *Host) : Host;
		OutOrigin = FString::Printf(TEXT("%s://%s:%d"), *Scheme, *DisplayHost, Port);
		return true;
	}

	inline bool CanSendCredentials(const FString& Url, bool bAllowInsecureRemote)
	{
		FString Origin; bool bLoopback = false;
		if (!TryGetOrigin(Url, Origin, bLoopback)) return false;
		return bLoopback || Origin.StartsWith(TEXT("https://")) || Origin.StartsWith(TEXT("wss://")) || bAllowInsecureRemote;
	}

	inline bool IsTrustedOrigin(const FString& Url, const FString& DefaultUrl, const TArray<FString>& AdditionalOrigins)
	{
		FString Origin, Trusted; bool bLoopback = false;
		if (!TryGetOrigin(Url, Origin, bLoopback)) return false;
		if (TryGetOrigin(DefaultUrl, Trusted, bLoopback) && Trusted == Origin) return true;
		for (const FString& Allowed : AdditionalOrigins)
			if (TryGetOrigin(Allowed, Trusted, bLoopback) && Trusted == Origin) return true;
		return false;
	}
}
