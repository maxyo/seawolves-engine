// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario shipyard_repair: Сен-Пьер shipyard, opening quest finished, 200 000 pesos, a battered brig as flagship (hull 45%, sails 40%, a mast down, 4 cannons lost).
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Unlock();
	Scenario_Money(200000);
	// A class 4 ship: the shipwright also offers its sails and coat of arms (CanSetSailsGerald).
	Scenario_Flagship(SHIP_BRIG, "Scenario");
	Scenario_Damage(0.45, 0.4, 1, 4);
	Scenario_Place("FortFrance_Shipyard", "reload", "reload1");
}

void Scenario_Arrived()
{
}

void Scenario_Trigger()
{
}
