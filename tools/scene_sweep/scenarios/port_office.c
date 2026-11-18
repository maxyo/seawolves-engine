// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario port_office: Сен-Пьер port office, opening quest finished, 200 000 pesos, two companion ships to leave in port.
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Unlock();
	Scenario_Money(200000);
	Scenario_Companion("ScenarioLuggerCap", SHIP_LUGGER, "Mouette");
	Scenario_Companion("ScenarioBrigCap", SHIP_BRIG, "Faucon");
	Scenario_Place("FortFrance_PortOffice", "reload", "reload1");
}

void Scenario_Arrived()
{
}

void Scenario_Trigger()
{
}
