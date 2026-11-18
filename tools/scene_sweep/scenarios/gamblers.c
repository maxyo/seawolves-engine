// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario gamblers: Сен-Пьер tavern at noon, 50 000 pesos; a card player at table 1 (the hero before him) and a dice player at table 4.
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	// Through the door, then a step before the card player (Scenario_Arrived): standing on his sit_front place
	// (by its locator name) would keep him from offering his game.
	Scenario_Place("FortFrance_tavern", "reload", "reload1");
	Scenario_Money(50000);
	// The tavern's own habitue players (LAi_utilites.c, the drinkers at the sit_base places): the tavern fills only
	// the free places, so these two keep theirs.
	Scenario_Gambler("ScenarioCards", 1, 0);
	Scenario_Gambler("ScenarioDice", 4, 1);
}

// A habitue of the tavern who plays (quest.last_theme 1): cards (game 0) or dice (game 1), at sit_base<table>.
void Scenario_Gambler(string id, int table, int game)
{
	ref player = Scenario_Person(id, "citizen", FRANCE, "FortFrance_tavern", "sit", "sit_base" + table);
	player.City = "FortFrance";
	player.CityType = "citizen";
	player.Default = "FortFrance_tavern";
	player.Default.group = "sit";
	player.Default.ToLocator = "sit_front" + table;
	player.Money = 20000;
	LAi_SetLoginTime(player, 0.0, 24.0);
	LAi_SetSitType(player);
	LAi_group_MoveCharacter(player, GetNationNameByType(FRANCE) + "_citizens");
	player.quest.last_theme = 1;
	player.quest.last_theme_game = game;
	player.dialog.filename = "Habitue_dialog.c";
	player.dialog.currentnode = "first time";
	player.greeting = "player";
}

void Scenario_Arrived()
{
	ref cards = CharacterFromID("ScenarioCards");
	ref dice = CharacterFromID("ScenarioDice");
	Scenario_Trace("cards: " + GetFullName(cards) + " at " + cards.location.locator + " with " + cards.Money + ", dice: " +
		GetFullName(dice) + " at " + dice.location.locator + " with " + dice.Money);
	Scenario_FacePerson("ScenarioCards", 1.3);
}

void Scenario_Trigger()
{
}
