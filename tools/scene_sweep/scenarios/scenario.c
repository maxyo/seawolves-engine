// SPDX-License-Identifier: GPL-3.0-or-later
// Driver of the test scenarios (tools/scene_sweep/scenarios/README.md). Test tooling only: never shipped, never part of
// the game. Every scenario file includes it and defines the three hooks below.
//
// tools/scene_sweep/scenarios.py writes a migration file (PROGRAM/migrations/NNNN_scenario_<name>.c, the game's own
// hook that runs while a save loads, migrations.c ApplyMigrations) into a throw-away copy of the game: on the emulator
// its read-only overlay, on the host the scene sweep's hard-linked copy. Loading the scenario's start save then runs:
//
//   Scenario_Setup()    inside the load: the state (money, items, officers, ships, damage, quests), people at their
//                       places (Scenario_Person), the destination (Scenario_Place)
//   (once the loaded scene is on screen, the hero goes to the destination with the game's own quest reload,
//   DoQuestReloadToLocation, so the location is entered as the player enters it: its people, shops and doors)
//   Scenario_Arrived()  once the destination is on screen: what needs the loaded location
//   (the game is saved as "scenario-<name>" in the player's profile, with the game's own SaveEngineState)
//   Scenario_Trigger()  after the save: what only a device run needs (a screen the game opens at that point); not
//                       saved, and not run by the host scene sweep (its capture is the saved state: the sweep's
//                       script gets no frames while a game screen is open)
//
// Then the segment unloads itself. The driver keeps its state in pchar.ScenarioTest, removed before the save; after
// the save pchar.ScenarioTest.done says it is complete (the scene sweep's scenario:<name> scene waits for it).
// Progress and failures are traced as "Scenario <name>: ..." lines; a failure says "Scenario <name>: FAILED <reason>".

#define SCENARIO_SETTLE_MS	3000	// on screen this long before the save (people sit down, the HUD comes up)

// Called by the generated migration file inside ApplyMigrations; `trigger`: run Scenario_Trigger after the save.
void Scenario_Run(string name, bool trigger)
{
	DeleteAttribute(pchar, "ScenarioTest");
	pchar.ScenarioTest = name;
	pchar.ScenarioTest.trigger = 0;
	if (trigger) pchar.ScenarioTest.trigger = 1;
	pchar.ScenarioTest.step = "start";
	pchar.ScenarioTest.time = 0;
	pchar.ScenarioTest.destination = "";
	trace("Scenario " + name + ": setup (saved from " + seadogs_saveFrom + ", hero in " + pchar.location + ", money " + pchar.money +
		", Sharlie lock " + CheckAttribute(pchar, "questTemp.Sharlie.Lock") + ")");
	Scenario_Setup();
	if (CheckAttribute(pchar, "ScenarioTest.failed")) return;
	SetEventHandler("frame", "Scenario_Frame", 0);
}

string Scenario_Name()
{
	return pchar.ScenarioTest;
}

void Scenario_Fail(string reason)
{
	trace("Scenario " + Scenario_Name() + ": FAILED " + reason);
	pchar.ScenarioTest.failed = reason;
}

void Scenario_Trace(string text)
{
	trace("Scenario " + Scenario_Name() + ": " + text);
}

// The scene is on screen and the player could act: no loading picture, no screen, no dialogue.
bool Scenario_OnScreen()
{
	aref fader;
	if (GetEntity(fader, "fader")) return false;
	if (sti(InterfaceStates.Launched)) return false;
	if (DialogRun) return false;
	if (IsEntity(&worldMap)) return worldMap.isLoaded == "true";
	if (bSeaActive) return bSeaLoaded && !bAbordageStarted;
	return FindLoadedLocation() >= 0;
}

void Scenario_Frame()
{
	if (!CheckAttribute(pchar, "ScenarioTest.step"))
	{
		DelEventHandler("frame", "Scenario_Frame");
		return;
	}
	string step = pchar.ScenarioTest.step;
	if (step == "start")
	{
		if (!Scenario_OnScreen()) return;
		pchar.ScenarioTest.step = "arrive";
		string destination = pchar.ScenarioTest.destination;
		if (destination == "sea")
		{
			Scenario_ToSea();
			return;
		}
		if (destination == "" || FindLocation(destination) == FindLoadedLocation()) return;
		chrDisableReloadToLocation = false;
		sQuestNameAfterReload = "_";
		if (!DoQuestReloadToLocation(destination, pchar.ScenarioTest.destination.group, pchar.ScenarioTest.destination.locator, ""))
		{
			Scenario_Fail("the game refused the reload to " + destination);
			Scenario_Stop();
		}
		return;
	}
	if (step == "arrive")
	{
		if (!Scenario_OnScreen()) return;
		if (pchar.ScenarioTest.destination == "sea")
		{
			if (!bSeaActive) return;
		}
		else
		{
			if (pchar.ScenarioTest.destination != "" && FindLocation(pchar.ScenarioTest.destination) != FindLoadedLocation()) return;
		}
		if (bSeaActive) Scenario_Trace("arrived at sea");
		else Scenario_Trace("arrived in " + pchar.location);
		Scenario_Arrived();
		if (CheckAttribute(pchar, "ScenarioTest.failed")) { Scenario_Stop(); return; }
		pchar.ScenarioTest.step = "settle";
		pchar.ScenarioTest.time = 0;
		return;
	}
	if (step == "settle")
	{
		if (!Scenario_OnScreen())
		{
			pchar.ScenarioTest.time = 0;
			return;
		}
		pchar.ScenarioTest.time = sti(pchar.ScenarioTest.time) + GetDeltaTime();
		if (sti(pchar.ScenarioTest.time) < SCENARIO_SETTLE_MS) return;
		if (!CheckSaveGameEnabled())
		{
			Scenario_Fail("the game does not allow saving here");
			Scenario_Stop();
			return;
		}
		bool trigger = sti(pchar.ScenarioTest.trigger);
		Scenario_Save();
		if (trigger) Scenario_Trigger();
		Scenario_Stop();
	}
}

// The game's own save (seadogs.c SaveGame) under the name "scenario-<name>", synchronously: the driver's attributes
// are gone from the saved state and back right after it.
void Scenario_Save()
{
	string name = Scenario_Name();
	// The engine names every save record by the hero and the place (AndroidSaveIdentity, the application's
	// android_save_identity.c, loaded with its UI bootstrap). The host scene sweep runs without the application's UI:
	// run.py puts that file into the game copy and the scenario loads it.
	if (!SegmentIsLoaded("interface/android_ui_bootstrap.c") && !SegmentIsLoaded("interface/android_save_identity.c"))
	{
		if (!LoadSegment("interface/android_save_identity.c")) Scenario_Fail("no AndroidSaveIdentity: interface/android_save_identity.c did not load");
	}
	string record = "scenario-" + name;
	string saveName = "SAVE\" + PlayerProfile.name + "\" + record;
	SendMessage(&GameInterface, "ls", MSG_INTERFACE_DELETE_SAVE_FILE, record);
	string describer = GetSaveDataString(GetCurLocationName());
	DeleteAttribute(pchar, "ScenarioTest");
	pchar.version_number = 104;
	SaveEngineState(saveName);
	ISetSaveData(saveName, describer);
	pchar.ScenarioTest = name;
	pchar.ScenarioTest.step = "done";
	string where = pchar.location;
	if (bSeaActive) where = "sea";
	trace("Scenario " + name + ": saved " + saveName + " in " + where);
}

void Scenario_Stop()
{
	DelEventHandler("frame", "Scenario_Frame");
	if (!CheckAttribute(pchar, "ScenarioTest.failed")) pchar.ScenarioTest.done = true;
	string name = Scenario_Name();
	trace("Scenario " + name + ": done");
	UnloadSegment("scenarios/" + name + ".c");	// freed at the next frame
}

// ---- helpers: the game's own functions, so the states are the ones the game makes ----

// Where the hero goes once the loaded scene is on screen (a quest reload, as the game's quests move him), at noon:
// shops, offices and churches open, people about.
void Scenario_Place(string location, string group, string locator)
{
	if (FindLocation(location) < 0)
	{
		Scenario_Fail("no location " + location);
		return;
	}
	pchar.ScenarioTest.destination = location;
	pchar.ScenarioTest.destination.group = group;
	pchar.ScenarioTest.destination.locator = locator;
	SetCurrentTime(12, 0);
	Scenario_Trace("destination " + location + " " + group + "::" + locator);
}

void Scenario_Money(int amount)
{
	AddMoneyToCharacter(pchar, amount - sti(pchar.money));
}

// A character of the game's phantom kind at a locator of a location (placed when that location loads).
ref Scenario_Person(string id, string type, int nation, string location, string group, string locator)
{
	int index = NPC_GenerateCharacter(id, "citiz_45", "man", "man", 10, nation, -1, true, type);
	ref person = GetCharacter(index);
	SetNPCModelUniq(person, type, MAN);
	ChangeCharacterAddressGroup(person, location, group, locator);
	return person;
}

// A hired officer (scripts/officers.c LandEnc_OfficerHired), in the given officer slot ("" passenger only;
// "navigator", "boatswain", "cannoner", "doctor", "treasurer", "carpenter", or "fighter" for the first free one).
ref Scenario_Officer(string id, int rank, string post)
{
	ref officer = GetCharacter(NPC_GenerateCharacter(id, "citiz_46", "man", "man", rank, sti(pchar.nation), -1, true, "officer"));
	SetNPCModelUniq(officer, "officer", MAN);
	SetFantomParamFromRank(officer, rank, true);
	officer.Money = 0;
	AddPassenger(pchar, officer, false);
	officer.location = "None";
	officer.Dialog.Filename = "Enc_Officer_dialog.c";
	officer.Dialog.CurrentNode = "hired";
	officer.Payment = true;
	officer.DontClearDead = true;
	officer.loyality = MAX_LOYALITY;
	DeleteAttribute(officer, "LifeDay");
	SaveCurrentNpcQuestDateParam(officer, "HiredDate");
	SetCharacterRemovable(officer, true);
	if (post == "fighter")
	{
		SetOfficersIndex(pchar, -1, sti(officer.index));
		officer.fighter = true;
		LAi_SetOfficerType(officer);
	}
	else
	{
		// interface/character_all.c AcceptAddOfficer
		if (post != "")
		{
			pchar.Fellows.Passengers.(post) = sti(officer.index);
			officer.(post) = true;
			officer.isfree = 1;
		}
	}
	return officer;
}

// A companion captain with his own ship and full crew in the hero's squadron.
ref Scenario_Companion(string id, int shipType, string shipName)
{
	ref captain = GetCharacter(NPC_GenerateCharacter(id, "citiz_46", "man", "man", 15, sti(pchar.nation), -1, true, "officer"));
	SetNPCModelUniq(captain, "officer", MAN);
	SetFantomParamFromRank(captain, 15, true);
	captain.Ship.Type = GenerateShipExt(shipType, true, captain);
	SetBaseShipData(captain);
	captain.Ship.Name = shipName;
	SetCrewQuantityFull(captain);
	captain.Money = 0;
	captain.Payment = true;
	captain.loyality = MAX_LOYALITY;
	captain.Dialog.Filename = "Enc_Officer_dialog.c";
	captain.Dialog.CurrentNode = "hired";
	captain.location = "None";
	DeleteAttribute(captain, "LifeDay");
	SetCharacterRemovable(captain, true);
	SetShipRemovable(captain, true);
	SetCompanionIndex(pchar, -1, sti(captain.index));
	return captain;
}

// The end of the opening quest's lock, as Mishelle.c does when Michel lets the hero go: every island open, the port
// office, shipyard, smugglers and tavern officers available.
void Scenario_Unlock()
{
	if (!CheckAttribute(pchar, "questTemp.Sharlie.Lock")) return;
	for (int i = 0; i < MAX_ISLANDS; i++)
	{
		if (!CheckAttribute(&Islands[i], "hidden")) Island_SetReloadEnableGlobal(Islands[i].id, true);
	}
	DeleteAttribute(pchar, "questTemp.Sharlie.Lock");
	LocatorReloadEnterDisable("FortFrance_store", "reload2", false);
	Scenario_Trace("opening quest lock removed");
}

// At sea off <island> (its first sea entry, at its port: under its fort's guns) or, with "", on the open sea away
// from any island; entered with the game's quest sea login (quests.c QuestToSeaLogin_*); the enemies of
// Scenario_Enemy come along.
void Scenario_PlaceSea(string island)
{
	if (island != "" && FindIsland(island) < 0)
	{
		Scenario_Fail("no island " + island);
		return;
	}
	pchar.ScenarioTest.destination = "sea";
	pchar.ScenarioTest.destination.island = island;
	SetCurrentTime(12, 0);
	if (island == "") Scenario_Trace("destination the open sea");
	else Scenario_Trace("destination the sea off " + island);
}

// A hostile ship (its captain id, group id) ahead of the hero at the sea login, attacking him: the game's own
// pursuit setup (Group_SetTaskAttack, Group_LockTask) with a fantom captain of the given rank.
ref Scenario_Enemy(string id, int shipType, int rank)
{
	ref captain = GetCharacter(NPC_GenerateCharacter(id, "citiz_46", "man", "man", rank, PIRATE, -1, true, "soldier"));
	SetFantomParamFromRank(captain, rank, true);
	FantomMakeSmallSailor(captain, shipType, "", CANNON_TYPE_CANNON_LBS3, 15, 10, 15, 10, 10);
	captain.AlwaysEnemy = true;
	string group = id + "Group";
	Group_DeleteGroup(group);
	Group_FindOrCreateGroup(group);
	Group_SetType(group, "pirate");
	Group_AddCharacter(group, id);
	Group_SetGroupCommander(group, id);
	Group_SetTaskAttack(group, PLAYER_GROUP);
	Group_LockTask(group);
	pchar.ScenarioTest.enemies.(group) = id;
	return captain;
}

void Scenario_ToSea()
{
	string island = pchar.ScenarioTest.destination.island;
	DeleteAttribute(&questToSeaLoginer, "");
	QuestToSeaLogin_Prepare(0.0, 0.0, island);
	float x = 0.0;
	float z = 0.0;
	float heading = 0.0;
	if (island != "")
	{
		QuestToSeaLogin_PrepareLoc(island, "reload", "l1", false);
		ref seaIsland = GetIslandByID(island);
		x = stf(seaIsland.reload.l1.x);
		z = stf(seaIsland.reload.l1.z);
		heading = stf(seaIsland.reload.l1.ay);
	}
	aref enemies;
	makearef(enemies, pchar.ScenarioTest.enemies);
	for (int i = 0; i < GetAttributesNum(enemies); i++)
	{
		string group = GetAttributeName(GetAttributeN(enemies, i));
		ref enemyGroup = Group_FindOrCreateGroup(group);
		float distance = 400.0 + 150.0 * i;	// in cannon range (about 700 m), ahead of the hero
		enemyGroup.Pos.x = x + distance * sin(heading);
		enemyGroup.Pos.z = z + distance * cos(heading);
		enemyGroup.Pos.ay = heading + PI;
		QuestToSeaLogin_Add(group);
	}
	QuestToSeaLogin_Launch();
}

// The hero's own ship becomes a new one of this type (the game's ship generator), with a full crew.
void Scenario_Flagship(int shipType, string shipName)
{
	pchar.Ship.Type = GenerateShipExt(shipType, true, pchar);
	SetBaseShipData(pchar);
	pchar.Ship.Name = shipName;
	SetCrewQuantityFull(pchar);
	Scenario_Trace("flagship " + shipName + " type " + shipType);
}

// Battle damage on the hero's ship as the game records it: hull and sails at the given share, masts down (ship.masts,
// as quests break them), cannons destroyed (Borts.<bort>.damages.c<i> = 1).
void Scenario_Damage(float hull, float sails, int mastsDown, int cannonsLost)
{
	pchar.ship.HP = makeint(GetCharacterShipHP(pchar) * hull);
	if (sails < 1.0) pchar.ship.SP = makeint(GetCharacterShipSP(pchar) * sails);
	int i;
	if (mastsDown > 0)
	{
		for (i = 1; i <= 3; i++)
		{
			string mast = "mast" + i;
			pchar.ship.masts.(mast) = 0;
			if (i <= mastsDown) pchar.ship.masts.(mast) = 1;
		}
	}
	for (i = 0; i < cannonsLost; i++)
	{
		string cannon = "c" + i;
		pchar.Ship.Cannons.Borts.cannonl.damages.(cannon) = 1.0;
	}
	RecalculateCargoLoad(pchar);
	Scenario_Trace("damage: hull " + makeint(GetHullPercent(pchar)) + " percent, sails " + makeint(GetSailPercent(pchar)) + " percent");
}

// Records of real quests in the hero's journal (the game's AddQuestRecord), closed ones moved to the completed tab.
void Scenario_Quest(string quest, int records, bool closed)
{
	for (int i = 1; i <= records; i++) AddQuestRecord(quest, "" + i);
	if (closed) CloseQuestHeader(quest);
}

// The hero a step away from a person, turned to him: in reach to talk without standing on the person's own place
// (a habitue offers his game only while the place in front of his seat is free, Habitue_dialog.c CheckFreeSitFront).
void Scenario_FacePerson(string id, float distance)
{
	ref person = CharacterFromID(id);
	float px, py, pz, hx, hy, hz;
	GetCharacterPos(person, &px, &py, &pz);
	GetCharacterPos(pchar, &hx, &hy, &hz);
	float dx = hx - px;
	float dz = hz - pz;
	float length = sqrt(dx * dx + dz * dz);
	if (length < 0.01)
	{
		dx = 1.0;
		dz = 0.0;
		length = 1.0;
	}
	TeleportCharacterToPos(pchar, px + dx / length * distance, hy, pz + dz / length * distance);
	CharacterTurnByChr(pchar, person);
}
