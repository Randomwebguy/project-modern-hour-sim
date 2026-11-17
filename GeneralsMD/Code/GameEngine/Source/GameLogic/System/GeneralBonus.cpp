// SPDX-License-Identifier: GPL-3.0-or-later
// ue5 bridge: see GameLogic/GeneralBonus.h for the file format.
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <cstdio>
#include <cstring>

#include "Common/GlobalData.h"
#include "Common/KindOf.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/GeneralBonus.h"
#include "GameLogic/Object.h"

namespace
{
	void report(const char *what, const char *line)
	{
		printf("GENZH bonus %s: %s\n", what, line);
		fflush(stdout);
		DEBUG_LOG(("GENZH bonus %s: %s", what, line));
	}

	// ALL / - match everything; VEHICLE leaves out aircraft (aircraft are vehicles too).
	Bool parseKind(const char *kind, KindOfMaskType &mustBe, KindOfMaskType &mustNotBe)
	{
		mustBe.clear();
		mustNotBe.clear();
		if (!strcmp(kind, "-") || !stricmp(kind, "ALL"))
			return TRUE;
		if (!stricmp(kind, "INFANTRY"))
			mustBe.set(KINDOF_INFANTRY);
		else if (!stricmp(kind, "VEHICLE"))
		{
			mustBe.set(KINDOF_VEHICLE);
			mustNotBe.set(KINDOF_AIRCRAFT);
		}
		else if (!stricmp(kind, "AIRCRAFT"))
			mustBe.set(KINDOF_AIRCRAFT);
		else if (!stricmp(kind, "STRUCTURE"))
			mustBe.set(KINDOF_STRUCTURE);
		else if (!stricmp(kind, "DEFENSE"))
			mustBe.set(KINDOF_FS_BASE_DEFENSE);
		else
			return FALSE;
		return TRUE;
	}

	// One line of the bonus file; returns FALSE when it is not understood.
	Bool applyLine(Player *player, const char *type, const char *kind, Real value)
	{
		KindOfMaskType mustBe, mustNotBe;
		if (!parseKind(kind, mustBe, mustNotBe))
			return FALSE;
		const Real scalar = 1.0f + value / 100.0f;
		if (!stricmp(type, "cash"))
		{
			if (value > 0.0f)
				player->getMoney()->deposit((UnsignedInt)(value + 0.5f), FALSE, FALSE);
		}
		else if (!stricmp(type, "cost"))
			player->addGeneralBonus(Player::GENERAL_BONUS_COST, mustBe, mustNotBe, max(0.0f, scalar));
		else if (!stricmp(type, "time"))
			player->addGeneralBonus(Player::GENERAL_BONUS_TIME, mustBe, mustNotBe, max(0.01f, scalar));
		else if (!stricmp(type, "veteran"))
			player->addGeneralBonus(Player::GENERAL_BONUS_VETERANCY, mustBe, mustNotBe, (Real)max(0, min((Int)LEVEL_LAST, (Int)value)));
		else if (!stricmp(type, "promotion"))
			player->addSciencePurchasePoints((Int)value);
		else if (!stricmp(type, "income"))
			player->setSupplyIncomeScalar(max(0.0f, scalar));
		else if (!stricmp(type, "health"))
			player->addGeneralBonus(Player::GENERAL_BONUS_HEALTH, mustBe, mustNotBe, max(0.01f, scalar));
		else if (!stricmp(type, "damage"))
			player->addGeneralBonus(Player::GENERAL_BONUS_DAMAGE, mustBe, mustNotBe, max(0.0f, scalar));
		else if (!stricmp(type, "speed"))
			player->addGeneralBonus(Player::GENERAL_BONUS_SPEED, mustBe, mustNotBe, max(0.01f, scalar));
		else if (!stricmp(type, "power"))
			player->setSpecialPowerReloadScalar(max(0.01f, scalar));
		else
			return FALSE;
		return TRUE;
	}
}

void GeneralBonus::applyCommandLineBonuses()
{
	const AsciiString &path = TheGlobalData->m_generalBonusFile;
	if (path.isEmpty() || TheGlobalData->m_skirmishMap.isEmpty() || TheGlobalData->m_skirmishObserve)
		return;
	Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
	if (!local || !local->isPlayableSide())
		return;
	FILE *file = fopen(path.str(), "r");
	if (!file)
	{
		report("file not found", path.str());
		return;
	}

	Int applied = 0;
	char line[256];
	while (fgets(line, sizeof(line), file))
	{
		line[strcspn(line, "\r\n")] = 0;
		char type[32], kind[32];
		float value = 0.0f;
		const char *start = line;
		if (!memcmp(start, "\xEF\xBB\xBF", 3))
			start += 3; // UTF-8 byte order mark
		start += strspn(start, " \t");
		if (!*start || *start == '#' || *start == ';')
			continue;
		if (sscanf(start, "%31s %31s %f", type, kind, &value) == 3 && applyLine(local, type, kind, value))
		{
			report("applied", start);
			++applied;
		}
		else
			report("ignored", start);
	}
	fclose(file);

	// Objects that already exist (starting command center and units); later ones get the bonus in Object::initObject.
	if (local->hasGeneralBonus(Player::GENERAL_BONUS_HEALTH) || local->hasGeneralBonus(Player::GENERAL_BONUS_SPEED))
		for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
			if (obj->getControllingPlayer() == local)
				local->friend_applyGeneralBonusesForObject(obj);

	char summary[64];
	snprintf(summary, sizeof(summary), "%d effects for player %d", applied, (Int)local->getPlayerIndex());
	report("done", summary);
}
