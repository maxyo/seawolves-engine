// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario shipyard_repair: Сен-Пьер shipyard, opening quest finished, 200 000 pesos, a battered brig as flagship (hull 45%, 4 cannons lost).
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Unlock();
	Scenario_Money(200000);
	// A class 4 ship: the shipwright also offers its sails and coat of arms (CanSetSailsGerald).
	Scenario_Flagship(SHIP_BRIG, "Scenario");
	// Hull and cannons only: torn sails and fallen masts are records the sea battle writes (ship.sails, masts) that
	// the shipyard's partial repair works through; set without them the game could not repair them part way.
	Scenario_Damage(0.45, 1.0, 0, 4);
	Scenario_Place("FortFrance_Shipyard", "reload", "reload1");
}

void Scenario_Arrived()
{
}

void Scenario_Trigger()
{
}
