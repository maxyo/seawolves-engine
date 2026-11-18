// SPDX-License-Identifier: GPL-3.0-or-later
// Scenario maps: Сен-Пьер streets with the best map, the atlas with six island maps and both halves of a treasure map.
#include "scenarios/scenario.c"

void Scenario_Setup()
{
	TakeNItems(pchar, "Map_Best", 1);
	TakeNItems(pchar, "map_martiniqua", 1);
	TakeNItems(pchar, "map_dominica", 1);
	TakeNItems(pchar, "map_guad", 1);
	TakeNItems(pchar, "map_barbados", 1);
	TakeNItems(pchar, "map_trinidad", 1);
	TakeNItems(pchar, "map_antigua", 1);
	TakeNItems(pchar, "map_part1", 1);
	TakeNItems(pchar, "map_part2", 1);
}

// The atlas takes the island maps the way the item screens make it (characterUtilite.c RefreshEquippedMaps; it needs
// the hero's entity, so not inside the load).
void Scenario_Arrived()
{
	RefreshEquippedMaps(pchar);
	Scenario_Trace("atlas maps " + pchar.MapsAtlasCount);
}

void Scenario_Trigger()
{
}
