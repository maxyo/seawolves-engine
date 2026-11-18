// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario church: Сен-Пьер church at noon with 20 000 pesos, before the priest (dialogue, donation).
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Place("FortFrance_church", "reload", "reload1");
	Scenario_Money(20000);
}

// A step before the priest, turned to him.
void Scenario_Arrived()
{
	Scenario_FacePerson("FortFrance_Priest", 1.5);
}

void Scenario_Trigger()
{
}
