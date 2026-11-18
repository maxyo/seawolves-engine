// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario fleet: Сен-Пьер streets, opening quest finished, corvette with navigator, boatswain and a fighter, two loaded companions (ship exchange, transfer), the month's crew pay due right after the save (salary screen).
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
}

void Scenario_Arrived()
{
}

// The month's pay comes due now, as at a change of day (time_events.c SalaryNextDayUpdate opens the salary screen);
// after the save, so the save itself starts in the streets.
void Scenario_Trigger()
{
	NullCharacter.SalayPayMonth = -1;
	SalaryNextDayUpdate();
}
