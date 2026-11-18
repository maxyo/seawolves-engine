// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario overload: Сен-Пьер streets, the hero carrying more than he can (whisky casks) with the carrying totem_06 in his bag, not worn.
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Place("FortFrance_town", "reload", "reload1");
	TakeNItems(pchar, "totem_06", 1);
}

// Over the limit by about two casks: the game gives an overloaded hero the walk as his run (characters.c
// CheckAndSetOverloadMode); wearing totem_06 lifts the rule. Weighed here, with the hero's real items and skills.
void Scenario_Arrived()
{
	int casks = makeint((GetMaxItemsWeight(pchar) - GetItemsWeight(pchar)) / 10.0) + 2;
	if (casks < 1) casks = 1;
	TakeNItems(pchar, "cask_whisky", casks);
	CheckAndSetOverloadMode(pchar);
	Scenario_Trace("weight " + GetItemsWeight(pchar) + " of " + GetMaxItemsWeight(pchar) + ", run action " + pchar.actions.run);
}

void Scenario_Trigger()
{
}
