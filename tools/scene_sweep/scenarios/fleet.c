// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario fleet: Сен-Пьер streets, opening quest finished, corvette with navigator, boatswain and a fighter, two loaded companions (ship exchange, transfer), crew pay due at the next day.
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Unlock();
	Scenario_Money(100000);
	Scenario_Flagship(SHIP_CORVETTE, "Scenario");
	SetCharacterGoods(pchar, GOOD_FOOD, 300);
	Scenario_Officer("ScenarioNavigator", 12, "navigator");
	Scenario_Officer("ScenarioBoatswain", 12, "boatswain");
	Scenario_Officer("ScenarioFighter", 12, "fighter");
	ref first = Scenario_Companion("ScenarioBrigCap", SHIP_BRIG, "Faucon");
	SetCharacterGoods(first, GOOD_SUGAR, 200);
	SetCharacterGoods(first, GOOD_BALLS, 300);
	ref second = Scenario_Companion("ScenarioLuggerCap", SHIP_LUGGER, "Mouette");
	SetCharacterGoods(second, GOOD_RUM, 100);
	// The month's pay is due at the next change of day (time_events.c SalaryNextDayUpdate).
	NullCharacter.SalayPayMonth = -1;
}

void Scenario_Arrived()
{
}

void Scenario_Trigger()
{
}
