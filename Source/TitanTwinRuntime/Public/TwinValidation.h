// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#pragma once

#include "CoreMinimal.h"
#include "TitanTwinTypes.h"

namespace TitanTwin::Validation
{
	inline constexpr int32 MaxIdentifierChars = 128;
	inline constexpr int32 MaxUnitChars = 64;
	inline constexpr int32 MaxTextChars = 65536;

	/** Validate raw external text before interning it in Unreal's permanent name pool. */
	TITANTWINRUNTIME_API bool TryMakeName(
		const FString& Text,
		FName& OutName,
		int32 MaxChars = MaxIdentifierChars,
		bool bAllowNone = false);

	TITANTWINRUNTIME_API bool IsValidIdentifier(
		FStringView Value,
		int32 MaxChars = MaxIdentifierChars,
		bool bAllowEmpty = false);

	TITANTWINRUNTIME_API bool IsValidName(
		FName Value,
		int32 MaxChars = MaxIdentifierChars,
		bool bAllowNone = false);

	TITANTWINRUNTIME_API bool IsValidValue(const FTwinValue& Value);
	TITANTWINRUNTIME_API bool IsValidSample(const FTwinPropertySample& Sample);
}
