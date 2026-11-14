// Copyright 2026 TitanSim. Licensed under the MIT License (see LICENSE).

#include "TwinReplaySafety.h"

#include "Engine/World.h"

namespace
{
	TSet<TWeakObjectPtr<UWorld>> ActiveReplayWorlds;

	void RemoveInvalidWorlds()
	{
		for (auto Iterator = ActiveReplayWorlds.CreateIterator(); Iterator; ++Iterator)
		{
			if (!Iterator->IsValid())
			{
				Iterator.RemoveCurrent();
			}
		}
	}
}

void FTwinReplaySafety::SetLiveReplayActive(UWorld* World, bool bActive)
{
	check(IsInGameThread());
	RemoveInvalidWorlds();
	if (!World)
	{
		return;
	}

	if (bActive)
	{
		ActiveReplayWorlds.Add(TWeakObjectPtr<UWorld>(World));
	}
	else
	{
		ActiveReplayWorlds.Remove(TWeakObjectPtr<UWorld>(World));
	}
}

bool FTwinReplaySafety::IsLiveReplayActive(const UWorld* World)
{
	check(IsInGameThread());
	RemoveInvalidWorlds();
	return World
		&& ActiveReplayWorlds.Contains(
			TWeakObjectPtr<UWorld>(const_cast<UWorld*>(World)));
}
