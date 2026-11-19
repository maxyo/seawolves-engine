// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario questbook: Сен-Пьер streets with fourteen real quests in the journal, five of them completed; the run then closes one more with a record (its «Logbook Update» line leads to the completed tab).
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	Scenario_Quest("FMQ_Martinique", 2, false);
	Scenario_Quest("FMQ_Guadeloupe", 2, false);
	Scenario_Quest("FMQ_Nevis", 2, false);
	Scenario_Quest("FMQ_Tortuga", 2, false);
	Scenario_Quest("FMQ_Lesson", 2, true);
	Scenario_Quest("FMQ_Portpax", 2, false);
	Scenario_Quest("Portugal", 2, true);
	Scenario_Quest("Consumption", 2, false);
	Scenario_Quest("Roger_1", 2, false);
	Scenario_Quest("Trial", 2, true);
	Scenario_Quest("Saga", 2, false);
	Scenario_Quest("Terrapin", 2, true);
	Scenario_Quest("Caleuche", 2, false);
	AddQuestRecord("FalseTrace", "2");
	CloseQuestHeader("FalseTrace");
}

void Scenario_Arrived()
{
}

// As most FalseTrace records do: the record that ends a quest closes it, so its «Новая запись в судовом журнале!»
// line opens the journal on a quest that is on the completed tab.
void Scenario_Trigger()
{
	AddQuestRecord("Consumption", "3");
	CloseQuestHeader("Consumption");
}
