// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinDataSourceMapping.h"
#include "TwinValidation.h"

namespace
{
	struct FAutomaticMappingTemplateToken
	{
		bool bPlaceholder = false;
		FString Text;
	};

	bool ParseAutomaticMappingTemplate(
		const FString& Template,
		TArray<FAutomaticMappingTemplateToken>& OutTokens)
	{
		OutTokens.Reset();
		int32 Cursor = 0;
		while (Cursor < Template.Len())
		{
			const int32 OpenBrace = Template.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
			if (OpenBrace == INDEX_NONE)
			{
				OutTokens.Add({ false, Template.Mid(Cursor) });
				break;
			}

			if (OpenBrace > Cursor)
			{
				OutTokens.Add({ false, Template.Mid(Cursor, OpenBrace - Cursor) });
			}

			const int32 CloseBrace = Template.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenBrace + 1);
			if (CloseBrace == INDEX_NONE)
			{
				return false;
			}

			FString Placeholder = Template.Mid(OpenBrace + 1, CloseBrace - OpenBrace - 1);
			Placeholder.TrimStartAndEndInline();
			if (Placeholder.IsEmpty())
			{
				return false;
			}

			OutTokens.Add({ true, MoveTemp(Placeholder) });
			Cursor = CloseBrace + 1;
		}

		return !OutTokens.IsEmpty();
	}

	bool MatchesLiteralAt(
		const FString& Address,
		int32 Cursor,
		const FString& Literal,
		ESearchCase::Type SearchCase)
	{
		return Address.Mid(Cursor, Literal.Len()).Equals(Literal, SearchCase);
	}

	bool MatchAutomaticMappingTemplate(
		const FString& Address,
		const FTwinAutomaticMappingRule& Rule,
		FString& OutEntityId,
		FString& OutPropertyName)
	{
		OutEntityId.Reset();
		OutPropertyName.Reset();

		const FString Template = Rule.AddressTemplate.TrimStartAndEnd();
		if (Template.IsEmpty())
		{
			return false;
		}

		TArray<FAutomaticMappingTemplateToken> Tokens;
		if (!ParseAutomaticMappingTemplate(Template, Tokens))
		{
			return false;
		}

		int32 EntityPlaceholderCount = 0;
		int32 PropertyPlaceholderCount = 0;
		for (int32 Index = 0; Index < Tokens.Num(); ++Index)
		{
			if (!Tokens[Index].bPlaceholder)
			{
				continue;
			}

			if (Tokens[Index].Text.Equals(TEXT("entity"), ESearchCase::IgnoreCase))
			{
				++EntityPlaceholderCount;
			}
			else if (Tokens[Index].Text.Equals(TEXT("property"), ESearchCase::IgnoreCase))
			{
				++PropertyPlaceholderCount;
			}

			if (Index + 1 < Tokens.Num() && Tokens[Index + 1].bPlaceholder)
			{
				return false;
			}
		}

		if (EntityPlaceholderCount != 1 || PropertyPlaceholderCount != 1)
		{
			return false;
		}

		const ESearchCase::Type SearchCase = Rule.bCaseSensitive
			? ESearchCase::CaseSensitive
			: ESearchCase::IgnoreCase;
		int32 AddressCursor = 0;
		for (int32 TokenIndex = 0; TokenIndex < Tokens.Num(); ++TokenIndex)
		{
			const FAutomaticMappingTemplateToken& Token = Tokens[TokenIndex];
			if (!Token.bPlaceholder)
			{
				if (!MatchesLiteralAt(Address, AddressCursor, Token.Text, SearchCase))
				{
					return false;
				}
				AddressCursor += Token.Text.Len();
				continue;
			}

			int32 CaptureEnd = Address.Len();
			if (TokenIndex + 1 < Tokens.Num())
			{
				const FString& NextLiteral = Tokens[TokenIndex + 1].Text;
				CaptureEnd = Address.Find(*NextLiteral, SearchCase, ESearchDir::FromStart, AddressCursor);
				if (CaptureEnd == INDEX_NONE)
				{
					return false;
				}
			}

			if (CaptureEnd <= AddressCursor)
			{
				return false;
			}

			FString Captured = Address.Mid(AddressCursor, CaptureEnd - AddressCursor);
			Captured.TrimStartAndEndInline();
			if (Captured.IsEmpty())
			{
				return false;
			}

			if (Token.Text.Equals(TEXT("entity"), ESearchCase::IgnoreCase))
			{
				OutEntityId = MoveTemp(Captured);
			}
			else if (Token.Text.Equals(TEXT("property"), ESearchCase::IgnoreCase))
			{
				OutPropertyName = MoveTemp(Captured);
			}
			AddressCursor = CaptureEnd;
		}

		return AddressCursor == Address.Len()
			&& !OutEntityId.IsEmpty()
			&& !OutPropertyName.IsEmpty();
	}

	bool BuildMapping(
		const FTwinDataPoint& Point,
		const FString& EntityId,
		const FString& PropertyName,
		FTwinDataSourceMapping& OutMapping)
	{
		FTwinDataSourceMapping Mapping;
		Mapping.SourceId = Point.SourceId;
		Mapping.Address = Point.Address.TrimStartAndEnd();
		if (!TitanTwin::Validation::TryMakeName(EntityId, Mapping.EntityId)
			|| !TitanTwin::Validation::TryMakeName(PropertyName, Mapping.PropertyName))
		{
			return false;
		}

		FString Reason;
		if (!Mapping.IsValid(&Reason))
		{
			return false;
		}

		OutMapping = MoveTemp(Mapping);
		return true;
	}

	bool TryBuildLastTwoSegmentsMapping(
		const FTwinDataPoint& Point,
		FTwinDataSourceMapping& OutMapping)
	{
		FString MappingPath = Point.Address.TrimStartAndEnd();
		if (MappingPath.StartsWith(TEXT("ns="), ESearchCase::IgnoreCase))
		{
			int32 StringIdentifierIndex = INDEX_NONE;
			if (!MappingPath.FindChar(TEXT(';'), StringIdentifierIndex)
				|| !MappingPath.Mid(StringIdentifierIndex).StartsWith(TEXT(";s="), ESearchCase::IgnoreCase))
			{
				return false;
			}

			MappingPath.RightChopInline(StringIdentifierIndex + 3);
			MappingPath.TrimStartAndEndInline();
		}

		TArray<FString> Segments;
		MappingPath.ParseIntoArray(Segments, TEXT("/"), true);
		if (Segments.Num() < 2 && MappingPath.Contains(TEXT(".")))
		{
			Segments.Reset();
			MappingPath.ParseIntoArray(Segments, TEXT("."), true);
		}
		for (FString& Segment : Segments)
		{
			Segment.TrimStartAndEndInline();
		}
		Segments.RemoveAll([](const FString& Segment)
		{
			return Segment.IsEmpty();
		});

		return Segments.Num() >= 2
			&& BuildMapping(Point, Segments[Segments.Num() - 2], Segments.Last(), OutMapping);
	}
}

UTwinDataSourceSettings::UTwinDataSourceSettings()
{
	PropertyUnitRules.Add(TEXT("WeldCurrent"), TEXT("A"));
	PropertyUnitRules.Add(TEXT("WeldVoltage"), TEXT("V"));
	PropertyUnitRules.Add(TEXT("Temperature"), FName(TEXT("\u00B0" "C")));
	PropertyUnitRules.Add(TEXT("WireFeedSpeed"), TEXT("m/min"));
}

FName UTwinDataSourceSettings::ResolveUnit(
	FName PropertyName,
	FName IncomingUnit,
	bool bOverrideUnit,
	FName OverrideUnit) const
{
	if (!IncomingUnit.IsNone())
	{
		return IncomingUnit;
	}

	if (bOverrideUnit && !OverrideUnit.IsNone())
	{
		return OverrideUnit;
	}

	if (const FName* DefaultUnit = PropertyUnitRules.Find(PropertyName))
	{
		return *DefaultUnit;
	}

	return NAME_None;
}

bool UTwinDataSourceSettings::TryBuildAutomaticMapping(
	const FTwinDataPoint& Point,
	FTwinDataSourceMapping& OutMapping, bool bAllowFallback) const
{
	const FString Address = Point.Address.TrimStartAndEnd();
	for (const FTwinAutomaticMappingRule& Rule : AutomaticMappingRules)
	{
		if (!Rule.bEnabled
			|| (!Rule.SourceId.IsNone() && Rule.SourceId != Point.SourceId))
		{
			continue;
		}

		FString EntityId;
		FString PropertyName;
		if (MatchAutomaticMappingTemplate(Address, Rule, EntityId, PropertyName)
			&& BuildMapping(Point, EntityId, PropertyName, OutMapping))
		{
			return true;
		}
	}

	return bAllowFallback && bUseLastTwoSegmentsFallback
		&& TryBuildLastTwoSegmentsMapping(Point, OutMapping);
}
