// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario sea_prize: on the open sea, opening quest finished, corvette with a companion brig and full crews, a weak pirate tartane and lugger attacking; the run then boards the tartane (not saved): plunder, prize, cargo/crew transfer.
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Unlock();
	Scenario_Money(50000);
	Scenario_Flagship(SHIP_CORVETTE, "Scenario");
	SetCharacterGoods(pchar, GOOD_FOOD, 300);
	SetCharacterGoods(pchar, GOOD_BALLS, 500);
	SetCharacterGoods(pchar, GOOD_GRAPES, 300);
	Scenario_Officer("ScenarioFighter", 12, "fighter");
	Scenario_Companion("ScenarioBrigCap", SHIP_BRIG, "Faucon");
	Scenario_Enemy("ScenarioPirate1", SHIP_WAR_TARTANE, 3);
	Scenario_Enemy("ScenarioPirate2", SHIP_LUGGER, 4);
	// The open sea: at Сен-Пьер the fort sinks the pirates before they reach the hero.
	Scenario_PlaceSea("");
}

void Scenario_Arrived()
{
}

// After the save the run boards the war tartane at once, as the InstantBoarding perk does (BattleInterface.c,
// Sea_AbordageStartNow): the deck fight, the captain's cabin, then plunder and the prize.
void Scenario_Trigger()
{
	Sea_AbordageStartNow(SHIP_ABORDAGE, GetCharacterIndex("ScenarioPirate1"), true, true);
}
