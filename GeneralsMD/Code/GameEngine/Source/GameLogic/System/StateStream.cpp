// SPDX-License-Identifier: GPL-3.0-or-later
// ue5 bridge: see GameLogic/StateStream.h for the protocol.
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <winsock.h>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <vector>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "Common/AudioEventRTS.h"
#include "Common/BuildAssistant.h"
#include "Common/ActionManager.h"
#include "Common/SkirmishCheats.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/Module/SpecialAbilityUpdate.h"
#include "GameLogic/Module/BattlePlanUpdate.h"
#include "GameLogic/Module/OverchargeBehavior.h"
#include "Common/GameAudio.h"
#include "Common/Energy.h"
class Object;
extern const Object *TheZHBuildBlocker; // BuildAssistant.cpp
extern Bool TheZHBuildBlockerExit;      // the refusal was a factory exit lane
// MN <object> <amount>: income paid by a building (AutoDepositUpdate: oil derricks, black markets, the capture bonus),
// queued during the logic frame and sent with the next published frame (28 Oct 2026, the user: "+amount" over it).
static std::string s_moneyLines;
void ZHStreamScriptCounters(UnsignedInt frame); // campaign counters, at the end of this file
void ZHEvaSuperweaponReady(UnsignedInt frame); // superweapon-ready EVA, at the end of this file
void ZHAIPersonalities(UnsignedInt frame); // AI personalities, at the end of this file
void ZHStreamMoney(UnsignedInt objectId, Int amount)
{
	if (amount <= 0 || !TheGlobalData || TheGlobalData->m_streamStatePort <= 0)
		return;
	char line[64];
	snprintf(line, sizeof(line), "MN %u %d\n", objectId, amount);
	s_moneyLines += line;
}
// LF <unit> <container> <frames>: a unit the leaflet drop disabled (LeafletDropBehavior, DISABLED_EMP for <frames>), so the
// client can show who stopped fighting (2 Nov 2026, the user: the leaflet drop should show what it does).
void ZHStreamLeaflet(UnsignedInt objectId, UnsignedInt containerId, UnsignedInt frames)
{
	if (!TheGlobalData || TheGlobalData->m_streamStatePort <= 0)
		return;
	char line[64];
	snprintf(line, sizeof(line), "LF %u %u %u\n", objectId, containerId, frames);
	s_moneyLines += line;
}
// EV <event> <priority>: an EVA message the sim announces to its local player (Eva.cpp processPlayingMessages, with EVA's
// own priorities, check intervals and expiry), so the client's HUD raises the matching banner (4 Nov 2026, plan 2.5).
void ZHStreamEva(const char* name, UnsignedInt priority)
{
	if (!TheGlobalData || TheGlobalData->m_streamStatePort <= 0 || !name || !*name)
		return;
	char line[128];
	snprintf(line, sizeof(line), "EV %s %u\n", name, priority);
	s_moneyLines += line;
}
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Recorder.h"
#include "Common/ThingFactory.h"
#include "Common/NameKeyGenerator.h"
#include "Common/SpecialPower.h"
#include "Common/Upgrade.h"
#include "GameClient/ControlBar.h"
#include "Common/PlayerTemplate.h"
#include "Common/Science.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/ExperienceTracker.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameClient/FXList.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/PolygonTrigger.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/LaserUpdate.h"
#include "GameLogic/Module/MissileAIUpdate.h"
#include "GameLogic/Module/NeutronMissileUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/StateStream.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/Scripts.h"
#include "GameClient/GameText.h"
#include "Common/GameState.h"
#include "GameNetwork/ConnectionManager.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/NetworkInterface.h"

#pragma comment(lib, "ws2_32.lib")

namespace
{
	SOCKET s_client = INVALID_SOCKET;
	bool s_failed = false;
	std::unordered_set<UnsignedInt> s_known;
	std::chrono::steady_clock::time_point s_nextFrame;
	std::string s_buffer;
	Int s_playersSent = -1;
	struct TurretState { Real angle[MAX_TURRETS]; Real pitch[MAX_TURRETS]; };
	std::unordered_map<UnsignedInt, TurretState> s_turrets;
	struct LaserState { Coord3D start, end; Real scale; LaserState() { start.x = start.y = start.z = end.x = end.y = end.z = 0.0f; scale = -1.0f; } };
	std::unordered_map<UnsignedInt, LaserState> s_lasers;
	std::unordered_map<UnsignedInt, LaserState> s_drawLasers; // laser drawables without an object (keyed by drawable id)
	std::unordered_map<UnsignedInt, std::string> s_queues;     // factory id -> last Q record
	std::unordered_map<UnsignedInt, Int> s_construction;      // structure id -> last whole percent
	std::unordered_map<UnsignedInt, Int> s_owners;            // object id -> last controlling player index (OW)
	std::unordered_map<UnsignedInt, UnsignedInt> s_containers; // object id -> last container id, 0 none (CN)
	std::unordered_set<UnsignedInt> s_exitSent; // producers whose door and rally point went out (EX)
std::unordered_set<UnsignedInt> s_missileSent; // missiles whose target went out (MT)
std::unordered_map<UnsignedInt, Int> s_supply;            // supply dock id -> last box count

	bool s_waterSent = false;
	bool s_bridgesSent = false;

	// B <template> <fromX> <fromY> <fromZ> <toX> <toY> <toZ> <width>: map bridges (Roads.ini template), once.
	void publishBridges()
	{
		if (s_bridgesSent || !TheTerrainLogic)
			return;
		s_bridgesSent = true;
		char line[256];
		for (Bridge *bridge = TheTerrainLogic->getFirstBridge(); bridge; bridge = bridge->getNext())
		{
			const BridgeInfo *info = bridge->peekBridgeInfo();
			snprintf(line, sizeof(line), "B %s %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", bridge->getBridgeTemplateName().str(),
				info->from.x, info->from.y, info->from.z, info->to.x, info->to.y, info->to.z, info->bridgeWidth);
			s_buffer += line;
		}
	}

	// A <river 0|1> <pointCount> <x y z>...: map water areas (world units), once after the map is loaded.
	// Model condition summary for the client (animation and damage states), bit i = Bits[i].
	UnsignedInt conditionBits(const Object *obj)
	{
		static const ModelConditionFlagType Bits[] = {
			MODELCONDITION_MOVING, MODELCONDITION_ATTACKING, MODELCONDITION_FIRING_A, MODELCONDITION_FIRING_B,
			MODELCONDITION_FIRING_C, MODELCONDITION_DYING, MODELCONDITION_DAMAGED, MODELCONDITION_REALLY_DAMAGED,
			MODELCONDITION_RUBBLE, MODELCONDITION_ACTIVELY_CONSTRUCTING, MODELCONDITION_PARTIALLY_CONSTRUCTED,
			MODELCONDITION_PANICKING, MODELCONDITION_PRONE, MODELCONDITION_GARRISONED, MODELCONDITION_CARRYING,
			MODELCONDITION_DOCKING, MODELCONDITION_USING_WEAPON_A, MODELCONDITION_USING_WEAPON_B, MODELCONDITION_USING_WEAPON_C,
			MODELCONDITION_PREATTACK_A, MODELCONDITION_PREATTACK_B, MODELCONDITION_PREATTACK_C,
			MODELCONDITION_BETWEEN_FIRING_SHOTS_A, MODELCONDITION_BETWEEN_FIRING_SHOTS_B, MODELCONDITION_SPECIAL_CHEERING,
			MODELCONDITION_EXPLODED_FLAILING, MODELCONDITION_EXPLODED_BOUNCING, MODELCONDITION_CLIMBING,
			MODELCONDITION_PARACHUTING, MODELCONDITION_FREEFALL, MODELCONDITION_RAISING_FLAG, MODELCONDITION_CAPTURED };
		const Drawable *draw = obj->getDrawable();
		if (!draw)
			return 0;
		const ModelConditionFlags &flags = draw->getModelConditionFlags();
		UnsignedInt bits = 0;
		for (Int i = 0; i < (Int)ARRAY_SIZE(Bits); ++i)
			if (flags.test(Bits[i]))
				bits |= 1u << i;
		return bits;
	}

	void publishWater()
	{
		if (s_waterSent)
			return;
		s_waterSent = true;
		char line[64];
		// K <timeOfDay 1 morning .. 4 night> <weather 0 normal, 1 snowy>: the map's lighting set and weather.
		snprintf(line, sizeof(line), "K %d %d\n", (Int)TheGlobalData->m_timeOfDay, (Int)TheGlobalData->m_weather);
		s_buffer += line;
		for (const PolygonTrigger *area = PolygonTrigger::getFirstPolygonTrigger(); area; area = area->getNext())
		{
			if (!area->isWaterArea() || area->getNumPoints() < 3)
				continue;
			snprintf(line, sizeof(line), "A %d %d", area->isRiver() ? 1 : 0, area->getNumPoints());
			s_buffer += line;
			for (Int i = 0; i < area->getNumPoints(); ++i)
			{
				const ICoord3D *point = area->getPoint(i);
				snprintf(line, sizeof(line), " %d %d %d", point->x, point->y, point->z);
				s_buffer += line;
			}
			s_buffer += "\n";
		}
	}

	// Multiplayer (ZH2026 LAN / online):
	// CH <slot> <allies 0|1> <text>: a chat line this player received (UTF-8; the sender's slot, team-only or to all).
	// PN <slot> <player> <team> <human> <active> <connected> <name>: each slot's player, every second (defeat, quits).
	// NS <runahead frames> <frame rate>: the lockstep's delay, every second.   DS: the sims went out of sync (once).
	std::string Utf8(const UnicodeString &text)
	{
		const Int wide = text.getLength();
		if (wide <= 0)
			return std::string();
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.str(), wide, nullptr, 0, nullptr, nullptr);
		std::string out(bytes, '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.str(), wide, &out[0], bytes, nullptr, nullptr);
		for (char &c : out)
			if (c == '\n' || c == '\r')
				c = ' ';
		return out;
	}

	UnicodeString Wide(const std::string &text)
	{
		UnicodeString out;
		const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
		if (count <= 0)
			return out;
		std::wstring wide(count, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &wide[0], count);
		out.set(wide.c_str());
		return out;
	}

	void onChat(Int fromSlot, Int playerMask, const UnicodeString &text)
	{
		const std::string body = Utf8(text);
		s_buffer += "CH " + std::to_string(fromSlot) + ((playerMask & 0xFF) == 0xFF ? " 0 " : " 1 ") + body + "\n";
	}

	Bool s_sentDesync = FALSE;
	void publishNetwork(UnsignedInt frame)
	{
		if (!TheNetwork || !TheGameInfo || frame % LOGICFRAMES_PER_SECOND != 0)
			return;
		char line[256];
		for (Int slot = 0; slot < MAX_SLOTS; ++slot)
		{
			Player *player = ThePlayerList ? ThePlayerList->getPlayerFromSlotIndex(slot) : nullptr;
			const GameSlot *info = TheGameInfo->getConstSlot(slot);
			if (!player || !info || !info->isOccupied())
				continue;
			snprintf(line, sizeof(line), "PN %d %d %d %d %d %d ", slot, (Int)player->getPlayerIndex(), info->getTeamNumber(), info->isHuman() ? 1 : 0,
				player->isPlayerActive() ? 1 : 0, !info->isHuman() || TheNetwork->isPlayerConnected(slot) ? 1 : 0);
			s_buffer += line;
			s_buffer += Utf8(info->getName());
			s_buffer += "\n";
		}
		snprintf(line, sizeof(line), "NS %u %u\n", TheNetwork->getRunAhead(), TheNetwork->getFrameRate());
		s_buffer += line;
		if (!s_sentDesync && TheNetwork->sawCRCMismatch())
		{
			s_sentDesync = TRUE;
			s_buffer += "DS\n";
		}
	}

	// P <index> <colour AARRGGBB> <side>: once per player list change (colours and sides do not change in a game).
	void publishPlayers()
	{
		if (!ThePlayerList || ThePlayerList->getPlayerCount() == s_playersSent)
			return;
		s_playersSent = ThePlayerList->getPlayerCount();
		char line[256];
		for (Int i = 0; i < s_playersSent; ++i)
		{
			const Player *player = ThePlayerList->getNthPlayer(i);
			if (!player)
				continue;
			const AsciiString side = player->getSide();
			// 4th field (30 Oct): how the local player sees this one - E enemy, A ally (or self), N neutral (civilians) - for the
			// client's inspect card on enemy and neutral objects.
			const Player *local = ThePlayerList->getLocalPlayer();
			const Relationship rel = local && local != player && player->getDefaultTeam() ? local->getRelationship(player->getDefaultTeam()) : ALLIES;
			snprintf(line, sizeof(line), "P %d %08X %s %c\n", (Int)player->getPlayerIndex(), (UnsignedInt)player->getPlayerColor(),
				side.isEmpty() ? "-" : side.str(), rel == ENEMIES ? 'E' : rel == ALLIES ? 'A' : 'N');
			s_buffer += line;
		}
	}

	// X <fxList> <x> <y> <z> [<x2> <y2> <z2>]: an effect the game played (explosion, muzzle flash, debris...), in
	// the frame it happened; the client draws its own version. Hooked into FXList, so it sees every effect.
	// 28 Oct 2026 (the user: blast, smoke and fire tied to the muzzle, the impact radius and the power): a magnitude tail
	//   M <kind> <radius> <damage> <source>
	// kind W = a weapon's shot (FXList::s_magnitude, set by Weapon.cpp: primary damage radius, primary damage, the firing
	// unit or projectile), D = its projectile's detonation, O = an effect on an object (deaths, damage states: the
	// object's bounding radius and full health, its id), R = only the radius the caller passed.
	void onEffect(const FXList *fx, const Coord3D *primary, const Coord3D *secondary, const Object *primaryObj, Real overrideRadius)
	{
		if (s_client == INVALID_SOCKET || !fx || fx->getNameKey() == NAMEKEY_INVALID)
			return;
		char line[320];
		Int n;
		if (secondary)
			n = snprintf(line, sizeof(line), "X %s %.1f %.1f %.1f %.1f %.1f %.1f", TheNameKeyGenerator->keyToName(fx->getNameKey()).str(),
				primary->x, primary->y, primary->z, secondary->x, secondary->y, secondary->z);
		else
			n = snprintf(line, sizeof(line), "X %s %.1f %.1f %.1f", TheNameKeyGenerator->keyToName(fx->getNameKey()).str(), primary->x, primary->y, primary->z);
		if (n < 0 || n >= (Int)sizeof(line) - 64)
			return;
		const FXList::Magnitude &m = FXList::s_magnitude;
		if (m.set)
			snprintf(line + n, sizeof(line) - n, " M %c %.1f %.1f %u\n", m.detonation ? 'D' : 'W', m.radius, m.damage, m.source);
		else if (primaryObj)
		{
			const BodyModuleInterface *body = primaryObj->getBodyModule();
			snprintf(line + n, sizeof(line) - n, " M O %.1f %.1f %u\n", primaryObj->getGeometryInfo().getBoundingSphereRadius(),
				body ? body->getMaxHealth() : 0.0f, (UnsignedInt)primaryObj->getID());
		}
		else if (overrideRadius > 0.0f)
			snprintf(line + n, sizeof(line) - n, " M R %.1f 0 0\n", overrideRadius);
		else
			snprintf(line + n, sizeof(line) - n, "\n");
		s_buffer += line;
	}

	// N <audioEvent> <handle> <objectId> [<x> <y> <z>]: a sound the local player hears starts (positional when it has a
	// place; objectId 0 when not attached). NE <audioEvent> <objectId> [<x> <y> <z>]: the game stopped asking for it.
	// Headless audio never plays, so the game re-adds ambient and looping sounds every frame: a sound counts as running
	// while it is re-added each frame, and only its start and end are sent.
	std::unordered_map<std::string, std::string> s_audioNow, s_audioPrev; // key -> NE record

	void flushAudioEnds()
	{
		for (const auto &prev : s_audioPrev)
			if (s_audioNow.find(prev.first) == s_audioNow.end())
				s_buffer += prev.second;
		s_audioPrev.swap(s_audioNow);
		s_audioNow.clear();
	}

	void onAudio(const AudioEventRTS *event, AudioHandle handle)
	{
		if (s_client == INVALID_SOCKET || event->getEventName().isEmpty() || event->getEventName() == "NoSound")
			return;
		AudioEventRTS *mutableEvent = const_cast<AudioEventRTS *>(event);
		const Coord3D *at = mutableEvent->isPositionalAudio() ? mutableEvent->getPosition() : nullptr;
		// ZH2026 (4 Nov 2026, the user: the map's ambience all played at once, everywhere): a sound owned by a drawable (the
		// map's ambient emitters) has no position until the audio manager's next update - take its drawable's, and its
		// object as the owner when it has one, so the client places it.
		Coord3D where;
		UnsignedInt drawableOwner = 0;
		if (mutableEvent->isPositionalAudio() && (!at || (at->x == 0.0f && at->y == 0.0f)))
		{
			const DrawableID did = mutableEvent->getDrawableID();
			const ObjectID oid = mutableEvent->getObjectID();
			const Drawable *drawable = did != INVALID_DRAWABLE_ID && TheGameClient ? TheGameClient->findDrawableByID(did) : nullptr;
			const Object *object = oid != INVALID_ID ? TheGameLogic->findObjectByID(oid) : (drawable ? drawable->getObject() : nullptr);
			if (object)
				where = *object->getPosition();
			else if (drawable)
				where = *drawable->getPosition();
			if (object || drawable)
				at = &where;
			if (object && oid == INVALID_ID)
				drawableOwner = (UnsignedInt)object->getID();
		}
		char line[256];
		char key[192];
		snprintf(key, sizeof(key), "%s %u %.0f %.0f", event->getEventName().str(), (UnsignedInt)mutableEvent->getObjectID(), at ? at->x : 0.0f, at ? at->y : 0.0f);
		if (at)
			snprintf(line, sizeof(line), "NE %s %u %.1f %.1f %.1f\n", event->getEventName().str(), (UnsignedInt)mutableEvent->getObjectID(), at->x, at->y, at->z);
		else
			snprintf(line, sizeof(line), "NE %s %u\n", event->getEventName().str(), (UnsignedInt)mutableEvent->getObjectID());
		const bool running = s_audioPrev.count(key) || s_audioNow.count(key);
		s_audioNow[key] = line;
		if (running)
			return;
		const UnsignedInt owner = drawableOwner ? drawableOwner : (UnsignedInt)mutableEvent->getObjectID();
		if (at)
			snprintf(line, sizeof(line), "N %s %u %u %.1f %.1f %.1f\n", event->getEventName().str(), (UnsignedInt)handle, owner, at->x, at->y, at->z);
		else
			snprintf(line, sizeof(line), "N %s %u %u\n", event->getEventName().str(), (UnsignedInt)handle, owner);
		s_buffer += line;
	}

	bool connectClient(Int port)
	{
		WSADATA data;
		if (WSAStartup(MAKEWORD(1, 1), &data) != 0)
			return false;
		SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == INVALID_SOCKET)
			return false;
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = inet_addr("127.0.0.1");
		address.sin_port = htons((u_short)port);
		if (bind(listener, (sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 1) != 0)
		{
			closesocket(listener);
			return false;
		}
		printf("StateStream: waiting for a client on 127.0.0.1:%d\n", port);
		fflush(stdout);
		s_client = accept(listener, nullptr, nullptr);
		closesocket(listener);
		if (s_client == INVALID_SOCKET)
			return false;
		u_long nonBlocking = 1;
		ioctlsocket(s_client, FIONBIO, &nonBlocking); // commands are polled once per frame
		s_buffer = "H zhstate 1\n";
		// W <map path>: the rest of the line is the map (paths may contain spaces).
		s_buffer += "W ";
		// A loaded save plays its scratch copy of the map: the client needs the original one (its terrain and props).
		if (TheGameState && TheGameState->getSaveGameInfo()->pristineMapName.isNotEmpty())
			s_buffer += TheGameState->getSaveGameInfo()->pristineMapName.str();
		else
			s_buffer += TheGlobalData->m_mapName.str();
		s_buffer += "\n";
		s_nextFrame = std::chrono::steady_clock::now();
		// Close gracefully when the process exits (end of a headless replay), so the client
		// reads the last frames instead of a connection reset.
		atexit(StateStream::shutdown);
		FXList::s_eventHook = onEffect;
		ConnectionManager::s_chatHook = onChat;
		AudioManager::s_eventHook = onAudio;
		return true;
	}

	std::string s_input;
	Int s_localSent = -1;
	Int s_lastMoney = -1, s_lastPowerMade = -1, s_lastPowerUsed = -1;

	void reply(bool ok, const char *command, const char *detail)
	{
		char line[256];
		snprintf(line, sizeof(line), "R %s %s %s\n", ok ? "ok" : "error", command, detail);
		s_buffer += line;
	}

	Object *singleSelected(const std::vector<ObjectID> &selection)
	{
		return selection.size() == 1 ? TheGameLogic->findObjectByID(selection[0]) : nullptr;
	}

	// ZH2026 (4 Nov 2026, the user could build nothing after his first building): the dozer among the selected objects -
	// a box or double-click selection with a dozer and anything else was refused as "builder". Else the single selected one.
	Object *selectedBuilder(const std::vector<ObjectID> &selection)
	{
		for (ObjectID id : selection)
		{
			Object *obj = TheGameLogic->findObjectByID(id);
			if (obj && obj->isKindOf(KINDOF_DOZER))
				return obj;
		}
		return singleSelected(selection);
	}

	// ZH2026 (27 Oct): the state of a command button for the client's panel - 0 hidden, 1 greyed (missing upgrade,
	// science, disabled), 2 ready, 3 recharging/reloading (percent) - the parts of ControlBar::getCommandAvailability the
	// panel needs, without its GUI windows. Shortcut powers (general powers) look up the player's readiest source.
	Int buttonState(const CommandButton *button, Object *obj, Player *player, Int &percent)
	{
		percent = 100;
		if (!button || !player)
			return 0;
		const GUICommandType type = button->getCommandType();
		const SpecialPowerTemplate *power = button->getSpecialPowerTemplate();
		if (type == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT || type == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT)
			obj = power ? player->findMostReadyShortcutSpecialPowerOfType(power->getSpecialPowerType()) : nullptr;
		if (!obj)
			return 0;
		if (obj->isDisabledByType(DISABLED_UNMANNED) || obj->testScriptStatusBit(OBJECT_STATUS_SCRIPT_DISABLED))
			return 0;
		if (obj->hasSingleUseCommandBeenUsed())
			return 1;
		const Bool disabled = obj->isDisabled() && !(BitIsSet(button->getOptions(), IGNORES_UNDERPOWERED) &&
			obj->getDisabledFlags().test(DISABLED_UNDERPOWERED) && obj->getDisabledFlags().count() == 1);
		if (disabled && type != GUI_COMMAND_SELL && type != GUI_COMMAND_EVACUATE && type != GUI_COMMAND_EXIT_CONTAINER &&
			type != GUI_COMMAND_SET_RALLY_POINT && type != GUI_COMMAND_STOP && type != GUI_COMMAND_SWITCH_WEAPON)
			return 1;
		if (BitIsSet(button->getOptions(), NEED_UPGRADE))
		{
			const UpgradeTemplate *upgrade = button->getUpgradeTemplate();
			if (upgrade && ((upgrade->getUpgradeType() == UPGRADE_TYPE_PLAYER && !player->hasUpgradeComplete(upgrade)) ||
				(upgrade->getUpgradeType() == UPGRADE_TYPE_OBJECT && !obj->hasUpgrade(upgrade))))
				return 1;
		}
		for (size_t i = 0; i < button->getScienceVec().size(); ++i)
		{
			if (type != GUI_COMMAND_PURCHASE_SCIENCE && !player->hasScience(button->getScienceVec()[i]))
				return 1;
		}
		// 30 Oct (the user: upgrades "not working fully"): the rest of ControlBar::getCommandAvailability - general's
		// powers never learned are hidden, NOT_QUEUEABLE / MUST_BE_STOPPED buttons wait, upgrades already owned (5) or in
		// research (6) are not offered again, the active battle plan and overcharge show as checked (4).
		if (BitIsSet(button->getOptions(), NEED_SPECIAL_POWER_SCIENCE) && power && power->getRequiredScience() != SCIENCE_INVALID &&
			type != GUI_COMMAND_PURCHASE_SCIENCE && type != GUI_COMMAND_PLAYER_UPGRADE && type != GUI_COMMAND_OBJECT_UPGRADE &&
			!player->hasScience(power->getRequiredScience()))
			return 0;
		ProductionUpdateInterface *production = obj->getProductionUpdateInterface();
		if (production && production->firstProduction() && BitIsSet(button->getOptions(), NOT_QUEUEABLE))
			return 1;
		if (BitIsSet(button->getOptions(), MUST_BE_STOPPED) && obj->getAI() && obj->getAI()->isMoving())
			return 1;
		const Bool queueMaxed = production && production->getProductionCount() == (UnsignedInt)MAX_BUILD_QUEUE_BUTTONS;
		switch (type)
		{
		case GUI_COMMAND_SPECIAL_POWER:
		case GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT:
		case GUI_COMMAND_SPECIAL_POWER_CONSTRUCT:
		case GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT:
		{
			if (!power)
				return 0;
			if (power->getRequiredScience() != SCIENCE_INVALID && !player->hasScience(power->getRequiredScience()))
				return 1;
			SpecialPowerModuleInterface *module = obj->getSpecialPowerModule(power);
			if (!module)
				return 1;
			const Real ready = module->getPercentReady();
			percent = (Int)(ready * 100.0f);
			if (ready < 1.0f)
				return 3;
			if (SpecialAbilityUpdate *ability = obj->findSpecialAbilityUpdate(power->getSpecialPowerType()))
			{
				if (ability->isPowerCurrentlyInUse(button))
					return 1;
			}
			else if (power->getSpecialPowerType() == SPECIAL_CHANGE_BATTLE_PLANS)
			{
				static NameKeyType key_BattlePlanUpdate = NAMEKEY("BattlePlanUpdate");
				BattlePlanUpdate *plans = (BattlePlanUpdate *)obj->findUpdateModule(key_BattlePlanUpdate);
				if (plans && (plans->getCommandOption() & button->getOptions()))
					return 4;
			}
			return 2;
		}
		case GUI_COMMAND_PLAYER_UPGRADE:
		{
			const UpgradeTemplate *upgrade = button->getUpgradeTemplate();
			if (!upgrade)
				return 0;
			if (player->hasUpgradeComplete(upgrade))
				return 5;
			if (player->hasUpgradeInProduction(upgrade))
				return 6;
			return queueMaxed ? 1 : 2; // money: the client compares the cost itself
		}
		case GUI_COMMAND_OBJECT_UPGRADE:
		{
			const UpgradeTemplate *upgrade = button->getUpgradeTemplate();
			if (!upgrade || !production)
				return 1;
			if (obj->hasUpgrade(upgrade))
				return 5;
			if (production->isUpgradeInQueue(upgrade))
				return 6;
			if (!obj->affectedByUpgrade(upgrade))
				return 1;
			return queueMaxed ? 1 : 2;
		}
		case GUI_COMMAND_TOGGLE_OVERCHARGE:
			for (BehaviorModule **module = obj->getBehaviorModules(); *module; ++module)
			{
				OverchargeBehaviorInterface *overcharge = (*module)->getOverchargeBehaviorInterface();
				if (overcharge && overcharge->isOverchargeActive())
					return 4;
			}
			return 2;
		case GUI_COMMAND_STOP:
		{
			if (!BitIsSet(button->getOptions(), OPTION_ONE))
				return 2;
			// the strategy center's stop: only while bombarding (it stops the artillery)
			static NameKeyType key_BattlePlanUpdate = NAMEKEY("BattlePlanUpdate");
			BattlePlanUpdate *plans = (BattlePlanUpdate *)obj->findUpdateModule(key_BattlePlanUpdate);
			return plans && plans->getActiveBattlePlan() != PLANSTATUS_BOMBARDMENT ? 1 : 2;
		}
		case GUI_COMMAND_FIRE_WEAPON:
		{
			const Weapon *weapon = obj->getWeaponInWeaponSlot(button->getWeaponSlot());
			if (!weapon)
				return (button->getOptions() & USES_MINE_CLEARING_WEAPONSET) != 0 ? 2 : 1;
			if (weapon->getClipReloadTime(obj) == 0 || weapon->getStatus() == READY_TO_FIRE)
				return 2;
			percent = (Int)(weapon->getPercentReadyToFire() * 100.0f);
			return weapon->getStatus() == RELOADING_CLIP ? 3 : 2;
		}
		case GUI_COMMAND_EVACUATE:
		case GUI_COMMAND_EXIT_CONTAINER:
			return obj->getContain() && obj->getContain()->getContainCount() > 0 ? 2 : 1;
		case GUI_COMMAND_HACK_INTERNET:
		case GUI_COMMAND_SWITCH_WEAPON:
		case GUI_COMMAND_COMBATDROP:
		case GUI_COMMAND_EXECUTE_RAILED_TRANSPORT:
		case GUI_COMMAND_SELL:
		case GUI_COMMAND_GUARD:
		case GUI_COMMAND_GUARD_WITHOUT_PURSUIT:
		case GUI_COMMAND_GUARD_FLYING_UNITS_ONLY:
		case GUI_COMMAND_ATTACK_MOVE:
		case GUI_COMMAND_SET_RALLY_POINT:
			return 2;
		default:
			return 2; // builds and upgrades: canmake answers those
		}
	}

	// The first science of a purchase button the player can buy now (ControlBar's GUI_COMMAND_PURCHASE_SCIENCE).
	ScienceType purchasableScience(const CommandButton *button, const Player *player)
	{
		for (size_t i = 0; button && player && i < button->getScienceVec().size(); ++i)
		{
			const ScienceType st = button->getScienceVec()[i];
			if (!player->hasScience(st) && TheScienceStore->playerHasPrereqsForScience(player, st) &&
				TheScienceStore->getSciencePurchaseCost(st) <= player->getSciencePurchasePoints())
				return st;
		}
		return SCIENCE_INVALID;
	}

	// /cheat powers (1 Nov 2026, the user: the cheat must refresh the cooldown of every power): every special power of the
	// player ready now. General powers share one timer per player (SharedNSync, Player's ready-timer list), which the
	// module's own frame does not reach - both are set, as Player::addScience does for a new promotion.
	Int readyAllPowers(Player *player)
	{
		Int ready = 0;
		const UnsignedInt now = TheGameLogic->getFrame();
		for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			if (obj->getControllingPlayer() != player || obj->isEffectivelyDead())
				continue;
			for (BehaviorModule **module = obj->getBehaviorModules(); *module; ++module)
			{
				SpecialPowerModuleInterface *power = (*module)->getSpecialPower();
				if (!power)
					continue;
				const SpecialPowerTemplate *temp = power->getSpecialPowerTemplate();
				if (temp && temp->isSharedNSync())
					player->expressSpecialPowerReadyFrame(temp, now);
				power->setReadyFrame(now);
				++ready;
			}
		}
		return ready;
	}

	// /cheat sciences and /cheat build (1 Nov, the user: with every general upgrade open, all of them must be usable): the
	// top rank, every promotion of the three purchase sets, the sciences the general power bar's powers need (CIA
	// Intelligence is not a promotion), and every power ready.
	Int learnAllPromotions(Player *player)
	{
		player->setRankLevel(5);
		Int granted = 0;
		const PlayerTemplate *pt = player->getPlayerTemplate();
		if (!pt || !TheControlBar)
			return 0;
		const AsciiString sets[4] = { pt->getPurchaseScienceCommandSetRank1(), pt->getPurchaseScienceCommandSetRank3(),
			pt->getPurchaseScienceCommandSetRank8(), pt->getSpecialPowerShortcutCommandSet() };
		for (Int r = 0; r < 4; ++r)
		{
			const CommandSet *set = TheControlBar->findCommandSet(sets[r]);
			for (Int i = 0; set && i < 18; ++i)
			{
				const CommandButton *b = set->getCommandButton(i);
				if (!b)
					continue;
				std::vector<ScienceType> wanted = b->getScienceVec();
				if (b->getSpecialPowerTemplate() && b->getSpecialPowerTemplate()->getRequiredScience() != SCIENCE_INVALID)
					wanted.push_back(b->getSpecialPowerTemplate()->getRequiredScience());
				for (size_t k = 0; k < wanted.size(); ++k)
					granted += !player->hasScience(wanted[k]) && TheScienceStore->isScienceGrantable(wanted[k]) && player->grantScience(wanted[k]) ? 1 : 0;
			}
		}
		readyAllPowers(player);
		return granted;
	}

	// Client commands (one per line). They become the same GameMessages the original UI sends for the local
	// player, so lockstep, replays and the logic's own checks keep working.
	//   sel <id>...               select (new group); later commands act on it
	//   move|amove|guard <x> <y> <z>
	//   attack <id> | stop | sell
	//   build <template> <x> <y> <z> <angle>   one selected dozer/worker
	//   queue <template>                       one selected factory
	//   upgrade <upgrade>                      one selected building or unit
	//   power <CommandButton> [<x> <y> <z>]    special power (target position when the button needs one)
	//   rally <x> <y> <z>                      rally point of the selected factory
	// Viewer speed (replays and observing): 0 pauses, 1 real time, up to 8; set by the client's "speed <x>".
	float s_speed = 1.0f;
	// Replay viewer (4 Nov 2026, plan 5.1): whose eyes the replay shows - a player's index (his fog of war) or -1 for all
	// (no fog); "viewplayer <index>". A change sends the vision again.
	int s_viewPlayer = -1;
	bool s_visionDirty = false;
	// ZH2026 (27 Oct): after the first frame the sim holds until the client says "ready" (its loading screen has every
	// model, texture and shader in), so nobody's battle starts while a client is still loading. GENZH_NO_READY_WAIT=1 skips.
	bool s_waitReady = true;
	int s_readyFrames = 0;
	// ZH2026 (28 Oct, trailer capture): GENZH_STREAM_STEP=1 runs the logic in lockstep with the client - a frame only after
	// the client grants it ("step <n>"), so an offline render (fixed frame time, however slow) sees every logic frame at
	// its own time.
	const bool s_stepMode = getenv("GENZH_STREAM_STEP") != nullptr;
	int s_stepBudget = 0;

	void handleCommand(const std::string &text)
	{
		static std::vector<ObjectID> selection;
		std::istringstream in(text);
		std::string verb;
		in >> verb;
		if (verb.empty())
			return;
		if (verb == "step")
		{
			int frames = 1;
			in >> frames;
			s_stepBudget += frames > 0 ? frames : 0;
			return;
		}
		if (verb == "speed")
		{
			// speed <x>: viewer pacing (0 pause .. 8); only for replays and observers, a player's match keeps real time.
			// 2 Nov 2026 (the user: game speed in the skirmish setup and the battle menu): a single-player skirmish too.
			float speed = 1.0f;
			in >> speed;
			Player *viewer = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
			// Never in a LAN game: the other players' sims wait for this one in lockstep.
			const bool watching = TheRecorder->isPlaybackMode() || (viewer && viewer->isPlayerObserver() && TheGlobalData->m_lanSlot < 0) || (!TheNetwork && TheGlobalData->m_lanSlot < 0);
			if (!watching)
				return reply(false, "speed", "not-watching");
			s_speed = speed < 0.0f ? 0.0f : speed > 8.0f ? 8.0f : speed;
			char text[16];
			snprintf(text, sizeof(text), "%g", s_speed);
			return reply(true, "speed", text);
		}
		if (verb == "viewplayer")
		{
			// viewplayer <index|-1>: replays only - a player's own vision or the whole map
			int index = -1;
			in >> index;
			if (!TheRecorder || !TheRecorder->isPlaybackMode())
				return reply(false, "viewplayer", "not-replay");
			Player *who = index >= 0 && ThePlayerList ? ThePlayerList->getNthPlayer(index) : nullptr;
			s_viewPlayer = who ? index : -1;
			s_visionDirty = true;
			char text[16];
			snprintf(text, sizeof(text), "%d", s_viewPlayer);
			return reply(true, "viewplayer", text);
		}		if (verb == "chat")
		{
			// chat <all|allies> <text>: a chat line over the game's own network (multiplayer only).
			std::string scope, text;
			in >> scope;
			std::getline(in, text);
			if (!text.empty() && text[0] == ' ')
				text.erase(0, 1);
			if (!TheNetwork && !text.empty())
			{
				// Single-player skirmish (27 Oct, the user: typing cheats in the Enter chat did nothing): the original chat
				// box's /cheat commands (InGameChat.cpp handleInGameSlashCommands), answered as a system chat line (CH -1).
				Player *player = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
				// /cheat reveal [on|off] (29 Oct, the user: map reveal for play tests): the shroud opens for the local player for
				// good (enemies seen and targetable), or comes back. Single-player skirmish only, like the other cheats.
				if (text.rfind("/cheat reveal", 0) == 0)
				{
					const bool off = text.find(" off") != std::string::npos;
					std::string feedback;
					if (!player || !CanUseSkirmishCheatsNow() || !ThePartitionManager)
						feedback = "Cheats are available only in single-player Skirmish.";
					else
					{
						if (off)
							ThePartitionManager->undoRevealMapForPlayerPermanently(player->getPlayerIndex());
						else
							ThePartitionManager->revealMapForPlayerPermanently(player->getPlayerIndex());
						feedback = off ? "Map reveal: OFF" : "Map reveal: ON";
					}
					s_buffer += "CH -1 0 " + feedback + "\n";
					return reply(true, "chat", "cheat");
				}
				// /cheat rank [veteran|elite|heroic] (10 Nov 2026, the user: a cheat that raises the selected units' rank): the
				// local player's selected trainable units go one rank up, or straight to the named one.
				if (text.rfind("/cheat rank", 0) == 0 || text.rfind("/cheat promote", 0) == 0)
				{
					std::string feedback;
					if (!player || !CanUseSkirmishCheatsNow())
						feedback = "Cheats are available only in single-player Skirmish.";
					else
					{
						Int target = -1;
						if (text.find("heroic") != std::string::npos || text.find("hero") != std::string::npos || text.find("max") != std::string::npos || text.find(" 3") != std::string::npos)
							target = LEVEL_HEROIC;
						else if (text.find("elite") != std::string::npos || text.find(" 2") != std::string::npos)
							target = LEVEL_ELITE;
						else if (text.find("veteran") != std::string::npos || text.find(" 1") != std::string::npos)
							target = LEVEL_VETERAN;
						Int raised = 0, chosen = 0;
						for (ObjectID id : selection)
						{
							Object *object = TheGameLogic->findObjectByID(id);
							ExperienceTracker *tracker = object && !object->isEffectivelyDead() && object->getControllingPlayer() == player ? object->getExperienceTracker() : nullptr;
							if (!tracker)
								continue;
							++chosen;
							if (!tracker->isTrainable())
								continue;
							const Int now = (Int)tracker->getVeterancyLevel();
							const Int level = target >= 0 ? target : (now < LEVEL_HEROIC ? now + 1 : (Int)LEVEL_HEROIC);
							if (level > now)
							{
								tracker->setVeterancyLevel((VeterancyLevel)level);
								++raised;
							}
						}
						feedback = raised ? "Rank raised: " + std::to_string(raised) + " unit(s)" : chosen ? "The selected units are already at that rank (or cannot gain ranks)." : "Select your own units first.";
					}
					s_buffer += "CH -1 0 " + feedback + "\n";
					return reply(true, "chat", "cheat");
				}
				// /cheat powers | sciences (30 Oct, the user: "lets test super powers"): every special power and superweapon of
				// the local player ready now / the general's top rank with every promotion learned (the powers they unlock).
				if (text.rfind("/cheat powers", 0) == 0 || text.rfind("/cheat sciences", 0) == 0)
				{
					std::string feedback;
					if (!player || !CanUseSkirmishCheatsNow())
						feedback = "Cheats are available only in single-player Skirmish.";
					else if (text.rfind("/cheat powers", 0) == 0)
						feedback = "Special powers ready: " + std::to_string(readyAllPowers(player));
					else
						feedback = "General rank 5, promotions learned: " + std::to_string(learnAllPromotions(player)) + ", all powers ready";
					s_buffer += "CH -1 0 " + feedback + "\n";
					return reply(true, "chat", "cheat");
				}
				const SkirmishCheats::Request cheat = SkirmishCheats::Parse(text.c_str());
				std::string feedback;
				char line[160];
				if (cheat.command == SkirmishCheats::NotCheat)
					feedback = "Chat is for multiplayer. Cheats: /cheat money [N] | build [on/off] | fastbuild [on/off] | power [on/off] | reveal [on/off] | powers | sciences | rank [veteran/elite/heroic] | reset - /weather clear|overcast|rain|storm|cycle";
				else if (!player || !CanUseSkirmishCheatsNow())
					feedback = "Cheats are available only in single-player Skirmish.";
				else if (cheat.command == SkirmishCheats::Money)
				{
					const UnsignedInt current = player->getMoney()->countMoney();
					const UnsignedInt amount = current >= 1000000000u ? 0u : min(cheat.amount, 1000000000u - current);
					player->getMoney()->deposit(amount, FALSE, FALSE);
					snprintf(line, sizeof(line), "Skirmish cheat: added $%u.", amount);
					feedback = line;
				}
				else if (cheat.command == SkirmishCheats::FastBuild || cheat.command == SkirmishCheats::Power)
				{
					SetSkirmishCheat(cheat.command, cheat.enabled);
					if (cheat.command == SkirmishCheats::Power)
						player->onPowerBrownOutChange(!player->getEnergy()->hasSufficientPower());
					feedback = std::string(cheat.command == SkirmishCheats::FastBuild ? "Fast build: " : "Power support: ") + (cheat.enabled ? "ON" : "OFF");
				}
				else if (cheat.command == SkirmishCheats::Build)
				{
					// /cheat build [on|off] (1 Nov 2026, the user: the Particle Cannon and every structure buildable directly):
					// no prerequisites, instant construction, power for everything, and the general's promotions (powers and
					// buttons gated by a science).
					SetSkirmishCheat(SkirmishCheats::Build, cheat.enabled);
					SetSkirmishCheat(SkirmishCheats::FastBuild, cheat.enabled);
					SetSkirmishCheat(SkirmishCheats::Power, cheat.enabled);
					player->onPowerBrownOutChange(!player->getEnergy()->hasSufficientPower());
					Int granted = 0;
					if (cheat.enabled)
						granted = learnAllPromotions(player);
					feedback = cheat.enabled ? "Build all: ON (no prerequisites, fast build, power, promotions +" + std::to_string(granted) + ")" : "Build all: OFF";
				}
				else if (cheat.command == SkirmishCheats::Reset)
				{
					ResetSkirmishCheats();
					player->onPowerBrownOutChange(!player->getEnergy()->hasSufficientPower());
					feedback = "Skirmish cheat toggles reset.";
				}
				else
					feedback = "/cheat money [1-10000000] | build [on/off] | fastbuild [on/off] | power [on/off] | reset";
				s_buffer += "CH -1 0 " + feedback + "\n";
				return reply(cheat.command != SkirmishCheats::NotCheat, "chat", "cheat");
			}
			if (!TheNetwork || text.empty())
				return reply(false, "chat", "no-network");
			Int mask = 0xFF;
			Player *me = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
			if (scope == "allies" && me)
			{
				mask = 0;
				for (Int slot = 0; slot < MAX_SLOTS; ++slot)
				{
					Player *other = ThePlayerList->getPlayerFromSlotIndex(slot);
					if (other && (other == me || me->getRelationship(other->getDefaultTeam()) == ALLIES))
						mask |= 1 << slot;
				}
			}
			TheNetwork->sendChat(Wide(text.substr(0, 200)), mask);
			return reply(true, "chat", scope.c_str());
		}
		if (verb == "quit")
		{
			// quit: leave the battle like the game's own quit menu (GameLogic::quit without its menu): this player's army
			// is destroyed, then the game exit makes the network announce "leaving on frame N", so the others go on at
			// once instead of waiting out the disconnect timeout.
			if (!TheNetwork || !TheGameLogic->isInGame())
				return reply(false, "quit", "not-network");
			GameMessage *destruct = TheMessageStream->appendMessage(GameMessage::MSG_SELF_DESTRUCT);
			destruct->appendBooleanArgument(TRUE);
			TheGameLogic->exitGame();
			return reply(true, "quit", "network");
		}
		// the client's loading screen is done: before the playing check, a replay waits for it too (4 Nov 2026: replays sat
		// out the 4-minute hold)
		if (verb == "ready")
		{
			s_waitReady = false;
			return reply(true, "ready", "go");
		}
		Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		if (!local || !TheGameLogic->isInGame() || TheRecorder->isPlaybackMode())
		{
			reply(false, verb.c_str(), "not-playing");
			return;
		}
		if (verb == "sel")
		{
			selection.clear();
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
			msg->appendBooleanArgument(TRUE);
			UnsignedInt id;
			while (in >> id)
			{
				Object *obj = TheGameLogic->findObjectByID((ObjectID)id);
				if (obj && obj->getControllingPlayer() == local)
				{
					selection.push_back((ObjectID)id);
					msg->appendObjectIDArgument((ObjectID)id);
				}
			}
			char detail[32];
			snprintf(detail, sizeof(detail), "%d", (Int)selection.size());
			reply(true, "sel", detail);
			return;
		}
		if (verb == "move" || verb == "amove" || verb == "guard")
		{
			Coord3D at;
			if (!(in >> at.x >> at.y >> at.z))
				return reply(false, verb.c_str(), "args");
			// A click on a bridge deck: the client's ground trace reaches the river bed under it, and the pathfinder picks
			// the layer nearest the target's height - the water - so units stopped at the bank (27 Oct, the user: units
			// could not cross the bridge). A target under a bridge goes onto the deck.
			for (Bridge *bridge = TheTerrainLogic ? TheTerrainLogic->getFirstBridge() : nullptr; bridge; bridge = bridge->getNext())
			{
				if (bridge->isPointOnBridge(&at))
				{
					const Real deck = bridge->getBridgeHeight(&at, nullptr);
					if (deck > at.z)
						at.z = deck;
				}
			}
			GameMessage *msg = TheMessageStream->appendMessage(verb == "move" ? GameMessage::MSG_DO_MOVETO :
				verb == "amove" ? GameMessage::MSG_DO_ATTACKMOVETO : GameMessage::MSG_DO_GUARD_POSITION);
			msg->appendLocationArgument(at);
			if (verb == "guard")
				msg->appendIntegerArgument(GUARDMODE_NORMAL);
			return;
		}
		if (verb == "wmove")
		{
			// wmove <x> <y> <z> (3 Nov 2026, the user: queued orders): Shift+right-click adds a waypoint for the selection, the
			// original's waypoint mode (MSG_ADD_WAYPOINT): units finish the path point after point.
			Coord3D at;
			if (!(in >> at.x >> at.y >> at.z))
				return reply(false, "wmove", "args");
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_ADD_WAYPOINT);
			msg->appendLocationArgument(at);
			return;
		}
		if (verb == "save")
		{
			// save <name> (3 Nov 2026, the user: save and load): a normal save game of a single-player battle (skirmish or
			// campaign) in the profile's Save folder; the client loads it by starting the sim with -loadsave <file>.
			std::string name;
			std::getline(in, name);
			std::string file;
			for (char c : name)
				if (isalnum((unsigned char)c) || c == '-' || c == '_')
					file += c;
			if (TheNetwork || TheGlobalData->m_lanSlot >= 0)
				return reply(false, "save", "multiplayer");
			if (file.empty())
				file = "quicksave";
			file += ".sav";
			UnicodeString desc;
			desc.translate(AsciiString(name.c_str()));
			const SaveResult result = TheGameState->saveGame(AsciiString(file.c_str()), desc, SAVE_FILE_TYPE_NORMAL);
			return reply(result.saveCode == SC_OK, "save", result.filename.isEmpty() ? file.c_str() : result.filename.str());
		}
		if (verb == "attack" || verb == "fattack")
		{
			// fattack <id> (29 Oct): the original Ctrl+click force attack, for neutral targets (civilian buildings: the Fab
			// town's Chaos collapse) and own units; plain attack is refused on them.
			UnsignedInt id = 0;
			in >> id;
			GameMessage *msg = TheMessageStream->appendMessage(verb == "fattack" ? GameMessage::MSG_DO_FORCE_ATTACK_OBJECT : GameMessage::MSG_DO_ATTACK_OBJECT);
			msg->appendObjectIDArgument((ObjectID)id);
			return;
		}
		if (verb == "stop" || verb == "sell" || verb == "scatter" || verb == "formation")
		{
			TheMessageStream->appendMessage(verb == "stop" ? GameMessage::MSG_DO_STOP : verb == "sell" ? GameMessage::MSG_SELL
				: verb == "scatter" ? GameMessage::MSG_DO_SCATTER : GameMessage::MSG_CREATE_FORMATION);
			return;
		}
		if (verb == "act" || verb == "hint")
		{
			// act|hint <targetId>: the original right-click decision for the selection on a target (CommandXlat's context
			// command): resume construction, repair, capture, enter (garrison/transport), get repaired, dock, attack.
			// hint only names it (the client's cursor); act issues it.
			UnsignedInt id = 0;
			in >> id;
			Object *target = TheGameLogic->findObjectByID((ObjectID)id);
			const bool doIt = verb == "act";
			if (!target || selection.empty())
				return reply(false, verb.c_str(), "none");
			std::string action = "none";
			Object *actor = nullptr;
			for (ObjectID sid : selection)
			{
				Object *obj = TheGameLogic->findObjectByID(sid);
				if (!obj || obj == target || obj->isEffectivelyDead())
					continue;
				const char *found = nullptr;
				if (target->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) && TheActionManager->canResumeConstructionOf(obj, target, CMD_FROM_PLAYER))
					found = "resume";
				else if (TheActionManager->canRepairObject(obj, target, CMD_FROM_PLAYER))
					found = "repair";
				else if (obj->findSpecialPowerModuleInterface(SPECIAL_INFANTRY_CAPTURE_BUILDING) && TheActionManager->canCaptureBuilding(obj, target, CMD_FROM_PLAYER))
					found = "capture";
				else if (TheActionManager->canEnterObject(obj, target, CMD_FROM_PLAYER, CHECK_CAPACITY))
					found = target->getContain() && target->getContain()->isGarrisonable() ? "garrison" : "enter";
				else if (TheActionManager->canGetRepairedAt(obj, target, CMD_FROM_PLAYER))
					found = "getrepaired";
				else if (TheActionManager->canDockAt(obj, target, CMD_FROM_PLAYER))
					found = "dock";
				else if (obj->getRelationship(target) == ENEMIES && TheActionManager->getCanAttackObject(obj, target, CMD_FROM_PLAYER, ATTACK_NEW_TARGET) == ATTACKRESULT_POSSIBLE)
					found = "attack";
				else if (obj->getRelationship(target) == ENEMIES && TheActionManager->getCanAttackObject(obj, target, CMD_FROM_PLAYER, ATTACK_NEW_TARGET) == ATTACKRESULT_POSSIBLE_AFTER_MOVING)
					found = "attack";
				if (found)
				{
					action = found;
					actor = obj;
					break;
				}
			}
			if (action == "none")
			{
				// Why nothing: the first selected object against the target (dozer, same player, under construction, the
				// builder's task and target, the target's shroud for the actor, the relationship).
				Object *first = selection.empty() ? nullptr : TheGameLogic->findObjectByID(selection[0]);
				char why[200] = "none";
				if (first && first != target)
				{
					Object *builder = TheGameLogic->findObjectByID(target->getBuilderID());
					DozerAIInterface *dozer = builder && builder->getAI() ? builder->getAI()->getDozerAIInterface() : nullptr;
					SpecialPowerModuleInterface *capture = first->findSpecialPowerModuleInterface(SPECIAL_INFANTRY_CAPTURE_BUILDING);
					snprintf(why, sizeof(why), "none:dozer=%d,same=%d,uc=%d,builder=%u,task=%d,tasktarget=%u,shroud=%d,rel=%d,capture=%d,ready=%.2f",
						first->isKindOf(KINDOF_DOZER) ? 1 : 0, first->getControllingPlayer() == target->getControllingPlayer() ? 1 : 0,
						target->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) ? 1 : 0, (unsigned)target->getBuilderID(),
						dozer ? (Int)dozer->getCurrentTask() : -1, dozer ? (unsigned)dozer->getTaskTarget(DOZER_TASK_BUILD) : 0u,
						first->getControllingPlayer() ? (Int)target->getShroudedStatus(first->getControllingPlayer()->getPlayerIndex()) : -1,
						(Int)first->getRelationship(target), capture ? 1 : 0, capture ? (double)capture->getPercentReady() : -1.0);
				}
				return reply(false, verb.c_str(), why);
			}
			if (!doIt)
				return reply(true, verb.c_str(), action.c_str());
			GameMessage *msg = nullptr;
			if (action == "resume")
			{
				msg = TheMessageStream->appendMessage(GameMessage::MSG_RESUME_CONSTRUCTION);
				msg->appendObjectIDArgument(target->getID());
			}
			else if (action == "repair")
			{
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_REPAIR);
				msg->appendObjectIDArgument(target->getID());
			}
			else if (action == "capture")
			{
				SpecialPowerModuleInterface *power = actor->findSpecialPowerModuleInterface(SPECIAL_INFANTRY_CAPTURE_BUILDING);
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT);
				msg->appendIntegerArgument(power->getSpecialPowerTemplate()->getID());
				msg->appendObjectIDArgument(target->getID());
				msg->appendIntegerArgument(NEED_TARGET_ENEMY_OBJECT | NEED_TARGET_NEUTRAL_OBJECT);
				msg->appendObjectIDArgument(INVALID_ID);
			}
			else if (action == "garrison" || action == "enter")
			{
				msg = TheMessageStream->appendMessage(GameMessage::MSG_ENTER);
				msg->appendObjectIDArgument(INVALID_ID); // the selection team
				msg->appendObjectIDArgument(target->getID());
			}
			else if (action == "getrepaired")
			{
				msg = TheMessageStream->appendMessage(GameMessage::MSG_GET_REPAIRED);
				msg->appendObjectIDArgument(target->getID());
			}
			else if (action == "dock")
			{
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DOCK);
				msg->appendObjectIDArgument(target->getID());
			}
			else if (action == "attack")
			{
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_ATTACK_OBJECT);
				msg->appendObjectIDArgument(target->getID());
			}
			return reply(true, "act", action.c_str());
		}
		if (verb == "canmake")
		{
			// ZH2026 (27 Oct): canmake <template>... - can the single selected builder make each (0 yes, 1 no prerequisite,
			// 2 no money, 3 factory disabled, 4 queue full, 5 parking full, 6 at the player's limit; 99 unknown). The client
			// greys its build buttons with it.
			Object *builder = selectedBuilder(selection);
			std::string out = "R ok canmake";
			std::string name;
			while (in >> name)
			{
				const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(name.c_str()));
				const int code = (!what || !builder) ? 99 : (int)TheBuildAssistant->canMakeUnit(builder, what);
				out += " " + name + ":" + std::to_string(code);
				// 1 (prerequisite): the missing structures, any of them per requirement: name:1:A|B,C
				if (code == 1 && what && builder && builder->getControllingPlayer())
				{
					std::string missing;
					for (Int i = 0; i < what->getPrereqCount(); ++i)
					{
						const ProductionPrerequisite *pre = what->getNthPrereq(i);
						if (pre->isSatisfied(builder->getControllingPlayer()))
							continue;
						const ThingTemplate *any[8];
						const Int n = pre->getAllPossibleBuildFacilityTemplates(any, 8);
						std::string group;
						for (Int k = 0; k < n; ++k)
							group += (k ? "|" : "") + std::string(any[k]->getName().str());
						if (!group.empty())
							missing += (missing.empty() ? "" : ",") + group;
					}
					if (!missing.empty())
						out += ":" + missing;
				}
			}
			s_buffer += out + "\n";
			return;
		}
		if (verb == "canbuild")
		{
			// ZH2026 (27 Oct): canbuild <template> <x> <y> <z> <angle> - the same legality test as build, without building
			// (the client tints its placement preview).
			std::string name;
			Coord3D at;
			Real angle = 0.0f;
			if (!(in >> name >> at.x >> at.y >> at.z >> angle))
				return reply(false, "canbuild", "args");
			const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(name.c_str()));
			Object *builder = selectedBuilder(selection);
			if (!what || !builder)
				return reply(false, "canbuild", !what ? "template" : "builder");
			static const char *const Reasons[] = { "ok", "location-terrain", "location-not-flat", "location-objects", "location-no-path",
				"location-shroud", "location-supplies", "location-other" };
			TheZHBuildBlocker = nullptr; // a refusal without a named blocker must not show the previous one
			TheZHBuildBlockerExit = FALSE;
			const LegalBuildCode legal = TheBuildAssistant->isLocationLegalToBuild(&at, what, angle,
				BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::CLEAR_PATH | BuildAssistant::NO_OBJECT_OVERLAP |
				BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::SHROUD_REVEALED, builder, local);
			if (legal == LBC_OBJECTS_IN_THE_WAY && TheZHBuildBlocker && TheZHBuildBlocker->getTemplate())
			{
				// name the blocker: location-objects:<template>, or location-exit:<template> when the site would block a
				// factory's exit lane (the original rule: a building that makes units keeps its door clear).
				char detail[160];
				snprintf(detail, sizeof(detail), TheZHBuildBlockerExit ? "location-exit:%s" : "location-objects:%s", TheZHBuildBlocker->getTemplate()->getName().str());
				return reply(false, "canbuild", detail);
			}
			return reply(legal == LBC_OK, "canbuild", Reasons[legal >= 0 && legal <= LBC_GENERIC_FAILURE ? legal : LBC_GENERIC_FAILURE]);
		}
		if (verb == "build")
		{
			std::string name;
			Coord3D at;
			Real angle = 0.0f;
			if (!(in >> name >> at.x >> at.y >> at.z >> angle))
				return reply(false, "build", "args");
			std::string queued; // "queue" (30 Oct): Shift-placed, the dozer takes it after its current building
			in >> queued;
			const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(name.c_str()));
			Object *builder = selectedBuilder(selection);
			if (!what || !builder)
				return reply(false, "build", !what ? "template" : "builder");
			if (TheBuildAssistant->canMakeUnit(builder, what) != CANMAKE_OK)
				return reply(false, "build", "cannot-make");
			const LegalBuildCode legal = TheBuildAssistant->isLocationLegalToBuild(&at, what, angle,
				BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::CLEAR_PATH | BuildAssistant::NO_OBJECT_OVERLAP |
				BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::SHROUD_REVEALED, builder, local);
			// ZH2026 (27 Oct): say why, so the client can tell the player (and tint the placement preview).
			static const char *const Reasons[] = { "ok", "location-terrain", "location-not-flat", "location-objects", "location-no-path",
				"location-shroud", "location-supplies", "location-other" };
			if (legal != LBC_OK)
				return reply(false, "build", Reasons[legal >= 0 && legal <= LBC_GENERIC_FAILURE ? legal : LBC_GENERIC_FAILURE]);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
			msg->appendIntegerArgument(what->getTemplateID());
			msg->appendLocationArgument(at);
			msg->appendRealArgument(angle);
			msg->appendBooleanArgument(queued == "queue");
			return reply(true, "build", name.c_str());
		}
		if (verb == "testspawn")
		{
			// testspawn <template> <x> <y> <z> [weapon-set bits: 1 player upgrade, 2 crate one, 4 crate two]:
			// verification only (client visual checks), refused unless the sim runs with GENZH_STREAM_TEST=1.
			static const bool allowed = getenv("GENZH_STREAM_TEST") != nullptr;
			std::string name;
			Coord3D at;
			unsigned int sets = 0;
			int healthPercent = 100;
			int ownerIndex = -1; // [player index]: another player's unit (staged battles for the trailer, 28 Oct)
			if (!allowed)
				return reply(false, "testspawn", "disabled");
			if (!(in >> name >> at.x >> at.y >> at.z))
				return reply(false, "testspawn", "args");
			in >> sets >> healthPercent >> ownerIndex;
			Player *owner = ownerIndex >= 0 && ThePlayerList ? ThePlayerList->getNthPlayer(ownerIndex) : local;
			const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(name.c_str()));
			if (!what || !owner || !owner->getDefaultTeam())
				return reply(false, "testspawn", !what ? "template" : "player");
			Object *obj = TheThingFactory->newObject(what, owner->getDefaultTeam());
			if (!obj)
				return reply(false, "testspawn", "create");
			// Aircraft start in the air (a parked helicopter never takes off for a test); their locomotor holds the height.
			at.z = TheTerrainLogic->getGroundHeight(at.x, at.y) + (obj->isKindOf(KINDOF_AIRCRAFT) ? 120.0f : 0.0f);
			obj->setPosition(&at);
			if (sets & 1u) obj->setWeaponSetFlag(WEAPONSET_PLAYER_UPGRADE);
			if (sets & 2u) obj->setWeaponSetFlag(WEAPONSET_CRATEUPGRADE_ONE);
			if (sets & 4u) obj->setWeaponSetFlag(WEAPONSET_CRATEUPGRADE_TWO);
			// 8 / 16 / 32: veteran / elite / heroic rank (9 Nov 2026: the heroic Mehmetcik check, simtest/mehmetcik_hero_test.py)
			if ((sets & 56u) && obj->getExperienceTracker())
				obj->getExperienceTracker()->setVeterancyLevel((sets & 32u) ? LEVEL_HEROIC : (sets & 16u) ? LEVEL_ELITE : LEVEL_VETERAN);
			BodyModuleInterface *body = obj->getBodyModule();
			if (body && healthPercent > 0 && healthPercent < 100)
				body->internalChangeHealth(body->getMaxHealth() * healthPercent / 100.0f - body->getHealth());
			return reply(true, "testspawn", name.c_str());
		}
		if (verb == "testgarrison")
		{
			// testgarrison <unitId> <buildingId>: verification only (GENZH_STREAM_TEST=1): any player's unit enters a building
			// (an enemy-held house for the attack checks; the local player cannot order enemy units).
			static const bool allowed = getenv("GENZH_STREAM_TEST") != nullptr;
			UnsignedInt unitId = 0, buildingId = 0;
			if (!allowed || !(in >> unitId >> buildingId))
				return reply(false, "testgarrison", "disabled");
			Object *unit = TheGameLogic->findObjectByID((ObjectID)unitId);
			Object *building = TheGameLogic->findObjectByID((ObjectID)buildingId);
			if (!unit || !building || !unit->getAI())
				return reply(false, "testgarrison", "none");
			unit->getAI()->aiEnter(building, CMD_FROM_AI);
			return reply(true, "testgarrison", "ok");
		}
		if (verb == "testrank")
		{
			// testrank <level>: verification only (GENZH_STREAM_TEST=1): the local general's rank (promotion points follow).
			static const bool allowed = getenv("GENZH_STREAM_TEST") != nullptr;
			Int level = 0;
			if (!allowed || !local || !(in >> level))
				return reply(false, "testrank", "disabled");
			local->setRankLevel(level);
			return reply(true, "testrank", "ok");
		}
		if (verb == "desynctest")
		{
			// desynctest: verification of the out-of-sync alert only (GENZH_STREAM_TEST=1): one skill point for the local player
			// outside the lockstep (skill points are part of the player CRC), so this sim no longer matches the other players'.
			static const bool allowed = getenv("GENZH_STREAM_TEST") != nullptr;
			if (!allowed || !local)
				return reply(false, "desynctest", "disabled");
			local->addSkillPoints(1);
			return reply(true, "desynctest", "diverged");
		}
		if (verb == "testwin")
		{
			// testwin: verification only (end-of-match checks: result, stats, progression award), refused unless the sim
			// runs with GENZH_STREAM_TEST=1. Destroys every object of the local player's enemies, so the normal victory
			// conditions end the game.
			static const bool allowed = getenv("GENZH_STREAM_TEST") != nullptr;
			if (!allowed)
				return reply(false, "testwin", "disabled");
			if (!local)
				return reply(false, "testwin", "player");
			int killed = 0;
			for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
			{
				Player *owner = obj->getControllingPlayer();
				if (owner && owner != local && local->getRelationship(owner->getDefaultTeam()) == ENEMIES && !obj->isEffectivelyDead())
				{
					obj->kill();
					++killed;
				}
			}
			char text[32];
			snprintf(text, sizeof(text), "%d", killed);
			return reply(true, "testwin", text);
		}
		if (verb == "queue")
		{
			std::string name;
			in >> name;
			const ThingTemplate *what = TheThingFactory->findTemplate(AsciiString(name.c_str()));
			Object *factory = singleSelected(selection);
			ProductionUpdateInterface *production = factory ? factory->getProductionUpdateInterface() : nullptr;
			if (!what || !production)
				return reply(false, "queue", !what ? "template" : "factory");
			if (TheBuildAssistant->canMakeUnit(factory, what) != CANMAKE_OK || production->canQueueCreateUnit(what) != CANMAKE_OK)
				return reply(false, "queue", "cannot-make");
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
			msg->appendIntegerArgument(what->getTemplateID());
			msg->appendIntegerArgument(production->requestUniqueUnitID());
			return reply(true, "queue", name.c_str());
		}
		if (verb == "rally")
		{
			// rally <x> <y> <z>: rally point of the one selected factory
			Coord3D at;
			Object *factory = singleSelected(selection);
			if (!(in >> at.x >> at.y >> at.z) || !factory)
				return reply(false, "rally", "args");
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SET_RALLY_POINT);
			msg->appendObjectIDArgument(factory->getID());
			msg->appendLocationArgument(at);
			// The logic drops a rally point no infantry path reaches (GameLogicDispatch doSetRallyPoint): say so (27 Oct).
			LocomotorSet human;
			human.addLocomotor(TheLocomotorStore->findLocomotorTemplate(NAMEKEY("BasicHumanLocomotor")));
			Coord3D from = *factory->getPosition();
			if (ExitInterface *door = factory->getObjectExitInterface())
			{
				Coord3D outside;
				if (door->getNaturalRallyPoint(outside))
					from = outside;
			}
			const Bool path = TheAI->pathfinder()->clientSafeQuickDoesPathExist(human, &from, &at);
			return reply(path, "rally", path ? "set" : "no-path");
		}
		if (verb == "steer")
		{
			// steer <x> <y> <z> (1 Nov 2026, the user: while the Particle Cannon fires, a click anywhere near the beam steers it
			// there): every object of the local player with an overridable-destination power active (a firing uplink; a
			// Spectre circling) turns towards the point - the original MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION, so it goes
			// through the lockstep message stream (ParticleUplinkCannonUpdate::setSpecialPowerOverridableDestination drives the
			// beam at ManualDrivingSpeed).
			Coord3D at;
			if (!(in >> at.x >> at.y >> at.z))
				return reply(false, "steer", "args");
			Int sent = 0;
			for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
			{
				if (obj->getControllingPlayer() != local || !obj->findSpecialPowerWithOverridableDestinationActive(SPECIAL_PARTICLE_UPLINK_CANNON))
					continue;
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION);
				msg->appendLocationArgument(at);
				msg->appendIntegerArgument(SPECIAL_PARTICLE_UPLINK_CANNON);
				msg->appendObjectIDArgument(obj->getID());
				++sent;
			}
			return reply(sent > 0, "steer", sent ? "ok" : "none");
		}
		if (verb == "power")
		{
			// power <CommandButton> [<x> <y> <z>]: a special power button (A-10 strike, paradrop, ...), at a target
			// position when the button needs one; the source is the selected building/unit when there is one.
			std::string name;
			in >> name;
			const CommandButton *button = TheControlBar ? TheControlBar->findCommandButton(AsciiString(name.c_str())) : nullptr;
			const SpecialPowerTemplate *power = button ? button->getSpecialPowerTemplate() : nullptr;
			if (!power)
				return reply(false, "power", "button");
			Object *source = singleSelected(selection);
			Coord3D at;
			if (in >> at.x >> at.y >> at.z)
			{
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
				msg->appendIntegerArgument(power->getID());
				msg->appendLocationArgument(at);
				msg->appendRealArgument(0.0f);
				msg->appendObjectIDArgument(INVALID_ID);
				msg->appendIntegerArgument(button->getOptions());
				msg->appendObjectIDArgument(source ? source->getID() : INVALID_ID);
			}
			else
			{
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER);
				msg->appendIntegerArgument(power->getID());
				msg->appendIntegerArgument(button->getOptions());
				msg->appendObjectIDArgument(source ? source->getID() : INVALID_ID);
			}
			return reply(true, "power", name.c_str());
		}
		if (verb == "button")
		{
			// button <CommandButton> [obj <id> | pos <x> <y> <z>] (27 Oct): any control-bar button the way the original
			// ControlBar/CommandXlat issues it - weapon fire (Comanche rocket pods), weapon switch, special powers incl.
			// general powers from the shortcut bar, general's promotions, evacuate/exit, hack, overcharge, combat drop.
			std::string name, how;
			in >> name >> how;
			const CommandButton *button = TheControlBar ? TheControlBar->findCommandButton(AsciiString(name.c_str())) : nullptr;
			if (!button)
				return reply(false, "button", "unknown");
			UnsignedInt targetId = 0;
			Coord3D at = { 0, 0, 0 };
			const bool byObject = how == "obj" && (in >> targetId);
			const bool byPos = how == "pos" && (in >> at.x >> at.y >> at.z);
			Object *target = byObject ? TheGameLogic->findObjectByID((ObjectID)targetId) : nullptr;
			if (byObject && !target)
				return reply(false, "button", "target");
			if (byObject)
				at = *target->getPosition();
			// A click on an object for a button that only takes a position fires at the object's spot.
			const bool wantsObject = (button->getOptions() & COMMAND_OPTION_NEED_OBJECT_TARGET) != 0;
			const bool atObject = byObject && wantsObject;
			const bool atPos = byPos || (byObject && !wantsObject);
			const SpecialPowerTemplate *power = button->getSpecialPowerTemplate();
			GameMessage *msg = nullptr;
			switch (button->getCommandType())
			{
			case GUI_COMMAND_FIRE_WEAPON:
				if (atObject && !BitIsSet(button->getOptions(), ATTACK_OBJECTS_POSITION))
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_WEAPON_AT_OBJECT);
					msg->appendIntegerArgument(button->getWeaponSlot());
					msg->appendObjectIDArgument(target->getID());
					msg->appendIntegerArgument(button->getMaxShotsToFire());
				}
				else if (atObject || atPos)
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_WEAPON_AT_LOCATION);
					msg->appendIntegerArgument(button->getWeaponSlot());
					msg->appendLocationArgument(at);
					msg->appendIntegerArgument(button->getMaxShotsToFire());
					msg->appendObjectIDArgument(target ? target->getID() : INVALID_ID);
				}
				else
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_WEAPON);
					msg->appendIntegerArgument(button->getWeaponSlot());
					msg->appendIntegerArgument(button->getMaxShotsToFire());
				}
				break;
			case GUI_COMMAND_SWITCH_WEAPON:
				msg = TheMessageStream->appendMessage(GameMessage::MSG_SWITCH_WEAPONS);
				msg->appendIntegerArgument(button->getWeaponSlot());
				break;
			case GUI_COMMAND_SPECIAL_POWER:
			case GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT:
			case GUI_COMMAND_SPECIAL_POWER_CONSTRUCT: // 30 Oct: powers that place a structure (GLA Sneak Attack tunnel) - the
			case GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT: // original's placement sends the same at-location message
			{
				if (!power)
					return reply(false, "button", "power");
				const Bool shortcut = button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT ||
					button->getCommandType() == GUI_COMMAND_SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT;
				Object *source = shortcut
					? local->findMostReadyShortcutSpecialPowerOfType(power->getSpecialPowerType()) : singleSelected(selection);
				if (shortcut && !source)
					return reply(false, "button", "no-source");
				if (atObject)
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT);
					msg->appendIntegerArgument(power->getID());
					msg->appendObjectIDArgument(target->getID());
					msg->appendIntegerArgument(button->getOptions());
					msg->appendObjectIDArgument(source ? source->getID() : INVALID_ID);
				}
				else if (atPos)
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
					msg->appendIntegerArgument(power->getID());
					msg->appendLocationArgument(at);
					msg->appendRealArgument(0.0f);
					msg->appendObjectIDArgument(INVALID_ID);
					msg->appendIntegerArgument(button->getOptions());
					msg->appendObjectIDArgument(source ? source->getID() : INVALID_ID);
				}
				else
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER);
					msg->appendIntegerArgument(power->getID());
					msg->appendIntegerArgument(button->getOptions());
					msg->appendObjectIDArgument(source ? source->getID() : INVALID_ID);
				}
				break;
			}
			case GUI_COMMAND_PURCHASE_SCIENCE:
			{
				const ScienceType science = purchasableScience(button, local);
				if (science == SCIENCE_INVALID)
					return reply(false, "button", "science");
				msg = TheMessageStream->appendMessage(GameMessage::MSG_PURCHASE_SCIENCE);
				msg->appendIntegerArgument(science);
				break;
			}
			case GUI_COMMAND_EVACUATE:
				msg = TheMessageStream->appendMessage(GameMessage::MSG_EVACUATE);
				break;
			case GUI_COMMAND_EXIT_CONTAINER:
				if (!byObject)
					return reply(false, "button", "args");
				msg = TheMessageStream->appendMessage(GameMessage::MSG_EXIT);
				msg->appendObjectIDArgument(target->getID());
				break;
			case GUI_COMMAND_EXECUTE_RAILED_TRANSPORT:
				msg = TheMessageStream->appendMessage(GameMessage::MSG_EXECUTE_RAILED_TRANSPORT);
				break;
			case GUI_COMMAND_HACK_INTERNET:
				msg = TheMessageStream->appendMessage(GameMessage::MSG_INTERNET_HACK);
				break;
			case GUI_COMMAND_TOGGLE_OVERCHARGE:
				msg = TheMessageStream->appendMessage(GameMessage::MSG_TOGGLE_OVERCHARGE);
				break;
			case GUI_COMMAND_COMBATDROP:
				if (byObject)
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_COMBATDROP_AT_OBJECT);
					msg->appendObjectIDArgument(target->getID());
				}
				else if (byPos)
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_COMBATDROP_AT_LOCATION);
					msg->appendLocationArgument(at);
				}
				break;
			case GUI_COMMAND_GUARD:
			case GUI_COMMAND_GUARD_WITHOUT_PURSUIT:
			case GUI_COMMAND_GUARD_FLYING_UNITS_ONLY:
			{
				const Int mode = button->getCommandType() == GUI_COMMAND_GUARD ? GUARDMODE_NORMAL
					: button->getCommandType() == GUI_COMMAND_GUARD_WITHOUT_PURSUIT ? GUARDMODE_GUARD_WITHOUT_PURSUIT : GUARDMODE_GUARD_FLYING_UNITS_ONLY;
				if (byObject)
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_GUARD_OBJECT);
					msg->appendObjectIDArgument(target->getID());
					msg->appendIntegerArgument(mode);
				}
				else if (byPos)
				{
					msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_GUARD_POSITION);
					msg->appendLocationArgument(at);
					msg->appendIntegerArgument(mode);
				}
				break;
			}
			case GUI_COMMAND_SELL:
				msg = TheMessageStream->appendMessage(GameMessage::MSG_SELL);
				break;
			case GUI_COMMAND_STOP:
				msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_STOP);
				break;
			default:
				break;
			}
			return reply(msg != nullptr, "button", name.c_str());
		}
		if (verb == "panel")
		{
			// panel (27 Oct): what the client's command panel shows, as records -
			//   BS <id> <button>:<state>:<percent>...   the first selected object's command set (state: buttonState)
			//   SH <button>:<state>:<percent>...        the general powers shortcut bar
			//   GR <rank> <skillPoints> <nextRankAt> <rankFrom> <purchasePoints>   general's rank (in-match promotions)
			//   SCI <button>:<state>:<owned>/<levels>:<rank>... the promotion buttons of ranks 1, 3 and 8 (state 0 rank not
			//                                            reached, 1 no points / prerequisite, 2 buyable now, 4 all owned)
			std::string line;
			char item[160];
			Int percent = 100;
			Object *first = selection.empty() ? nullptr : TheGameLogic->findObjectByID(selection[0]);
			const CommandSet *set = first && TheControlBar ? TheControlBar->findCommandSet(first->getCommandSetString()) : nullptr;
			if (set)
			{
				snprintf(item, sizeof(item), "BS %u", (unsigned)first->getID());
				line = item;
				for (Int i = 0; i < 18; ++i)
				{
					const CommandButton *b = set->getCommandButton(i);
					if (!b)
						continue;
					const Int state = buttonState(b, first, local, percent);
					snprintf(item, sizeof(item), " %s:%d:%d", b->getName().str(), state, percent);
					line += item;
				}
				s_buffer += line + "\n";
			}
			// ST <id> <health> <max> <damage> <range> <speed> <veterancy> <xp> <xpNext>: the numbers of the first selected
			// object and of the object under the cursor ("panel <hoverId>"), any side (damage per shot of the primary weapon
			// with the unit's bonuses, range and speed in sim units, speed per second).
			UnsignedInt hoverId = 0;
			in >> hoverId;
			Object *hovered = hoverId ? TheGameLogic->findObjectByID((ObjectID)hoverId) : nullptr;
			Object *const shown[2] = { first, hovered != first ? hovered : nullptr };
			for (Object *who : shown)
			{
				if (!who)
					continue;
				const BodyModuleInterface *body = who->getBodyModule();
				const Weapon *weapon = who->getWeaponInWeaponSlot(PRIMARY_WEAPON);
				Real damage = 0.0f, range = 0.0f, speed = 0.0f;
				if (weapon)
				{
					damage = const_cast<Weapon *>(weapon)->estimateWeaponDamage(who, who->getPosition()); // reads only
					range = weapon->getAttackRange(who);
				}
				if (const AIUpdateInterface *ai = who->getAI())
				{
					if (const Locomotor *locomotor = ai->getCurLocomotor())
						speed = locomotor->getMaxSpeedForCondition(body ? body->getDamageState() : BODY_PRISTINE) * LOGICFRAMES_PER_SECOND;
				}
				const ExperienceTracker *xp = who->getExperienceTracker();
				const Int level = (Int)who->getVeterancyLevel();
				const Int next = level + 1 < LEVEL_COUNT ? who->getTemplate()->getExperienceRequired(level + 1) : 0;
				snprintf(item, sizeof(item), "ST %u %.0f %.0f %.0f %.0f %.1f %d %d %d\n", (unsigned)who->getID(), body ? body->getHealth() : 0.0f,
					body ? body->getMaxHealth() : 0.0f, damage, range, speed, level, xp ? xp->getCurrentExperience() : 0, next);
				s_buffer += item;
			}			// RY <id> <x> <y> <z> | RY <id> none: the rally point of the selected factory (the client plants its flag there).
			if (first && first->getControllingPlayer() == local)
			{
				if (ExitInterface *exit = first->getObjectExitInterface())
				{
					const Coord3D *rally = exit->getRallyPoint();
					if (rally)
						snprintf(item, sizeof(item), "RY %u %.1f %.1f %.1f\n", (unsigned)first->getID(), rally->x, rally->y, rally->z);
					else
						snprintf(item, sizeof(item), "RY %u none\n", (unsigned)first->getID());
					s_buffer += item;
				}
			}			const PlayerTemplate *pt = local->getPlayerTemplate();
			if (pt && TheControlBar)
			{
				if (const CommandSet *shortcuts = TheControlBar->findCommandSet(pt->getSpecialPowerShortcutCommandSet()))
				{
					line = "SH";
					for (Int i = 0; i < 18; ++i)
					{
						const CommandButton *b = shortcuts->getCommandButton(i);
						if (!b)
							continue;
						const Int state = buttonState(b, nullptr, local, percent);
						snprintf(item, sizeof(item), " %s:%d:%d", b->getName().str(), state, percent);
						line += item;
					}
					s_buffer += line + "\n";
				}
				snprintf(item, sizeof(item), "GR %d %d %d %d %d\n", local->getRankLevel(), local->getSkillPoints(), local->getSkillPointsLevelUp(),
					local->getSkillPointsLevelDown(), local->getSciencePurchasePoints());
				s_buffer += item;
				line = "SCI";
				const AsciiString rankSets[3] = { pt->getPurchaseScienceCommandSetRank1(), pt->getPurchaseScienceCommandSetRank3(), pt->getPurchaseScienceCommandSetRank8() };
				const Int rankNeeded[3] = { 1, 3, 8 };
				for (Int r = 0; r < 3; ++r)
				{
					const CommandSet *ranks = TheControlBar->findCommandSet(rankSets[r]);
					for (Int i = 0; ranks && i < 18; ++i)
					{
						const CommandButton *b = ranks->getCommandButton(i);
						if (!b || b->getCommandType() != GUI_COMMAND_PURCHASE_SCIENCE)
							continue;
						Int owned = 0;
						const Int levels = (Int)b->getScienceVec().size();
						for (Int s = 0; s < levels; ++s)
							owned += local->hasScience(b->getScienceVec()[s]) ? 1 : 0;
						const Int state = owned == levels ? 4 : local->getRankLevel() < rankNeeded[r] ? 0
							: purchasableScience(b, local) != SCIENCE_INVALID ? 2 : 1;
						snprintf(item, sizeof(item), " %s:%d:%d/%d:%d", b->getName().str(), state, owned, levels, rankNeeded[r]);
						line += item;
					}
				}
				s_buffer += line + "\n";
			}
			return;
		}
		if (verb == "upgrade")
		{
			// upgrade <UpgradeName>: researched by the one selected building (or bought by the selected unit)
			std::string name;
			in >> name;
			const UpgradeTemplate *upgrade = TheUpgradeCenter->findUpgrade(AsciiString(name.c_str()));
			Object *owner = singleSelected(selection);
			if (!upgrade || !owner)
				return reply(false, "upgrade", !upgrade ? "upgrade" : "building");
			// 30 Oct: say why an upgrade is not taken (the logic drops the message silently)
			ProductionUpdateInterface *production = owner->getProductionUpdateInterface();
			if (!production)
				return reply(false, "upgrade", "building");
			const Bool player = upgrade->getUpgradeType() == UPGRADE_TYPE_PLAYER;
			if (player ? local->hasUpgradeComplete(upgrade) : owner->hasUpgrade(upgrade))
				return reply(false, "upgrade", "owned");
			if (player ? local->hasUpgradeInProduction(upgrade) : production->isUpgradeInQueue(upgrade))
				return reply(false, "upgrade", "in-research");
			if (local->getMoney()->countMoney() < (UnsignedInt)upgrade->calcCostToBuild(local))
				return reply(false, "upgrade", "money");
			if (production->canQueueUpgrade(upgrade) != CANMAKE_OK)
				return reply(false, "upgrade", "queue-full");
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UPGRADE);
			msg->appendObjectIDArgument(owner->getID());
			msg->appendIntegerArgument(upgrade->getUpgradeNameKey());
			return reply(true, "upgrade", name.c_str());
		}
		if (verb == "cancel")
		{
			// cancel <template|upgrade> (30 Oct): the last queued one of that kind in the one selected factory, refunded -
			// the original's right click on a build button or a queue entry.
			std::string name;
			in >> name;
			Object *factory = singleSelected(selection);
			ProductionUpdateInterface *production = factory ? factory->getProductionUpdateInterface() : nullptr;
			if (!production || factory->getControllingPlayer() != local)
				return reply(false, "cancel", "factory");
			if (const UpgradeTemplate *upgrade = TheUpgradeCenter->findUpgrade(AsciiString(name.c_str())))
			{
				if (!production->isUpgradeInQueue(upgrade))
					return reply(false, "cancel", "not-queued");
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UPGRADE);
				msg->appendIntegerArgument(upgrade->getUpgradeNameKey());
				return reply(true, "cancel", name.c_str());
			}
			const ProductionEntry *last = nullptr;
			for (const ProductionEntry *entry = production->firstProduction(); entry; entry = production->nextProduction(entry))
			{
				if (entry->getProductionType() == PRODUCTION_UNIT && entry->getProductionObject() &&
					entry->getProductionObject()->getName() == AsciiString(name.c_str()))
					last = entry;
			}
			if (!last)
				return reply(false, "cancel", "not-queued");
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UNIT_CREATE);
			msg->appendIntegerArgument(last->getProductionID());
			return reply(true, "cancel", name.c_str());
		}
		reply(false, verb.c_str(), "unknown");
	}

	void pollCommands()
	{
		char data[4096];
		for (;;)
		{
			const int got = recv(s_client, data, sizeof(data), 0);
			if (got <= 0)
				break;
			s_input.append(data, got);
		}
		size_t end;
		while ((end = s_input.find('\n')) != std::string::npos)
		{
			std::string line = s_input.substr(0, end);
			s_input.erase(0, end + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			handleCommand(line);
		}
	}

	std::string s_lastVision;
	Int s_resultSent = 0;

	// G <victory|defeat>: the local player's game result, once, followed by
	// GS <unitsDestroyed> <structuresDestroyed> <unitsBuilt> <moneyEarned> <gameSeconds>: the local player's match stats.
	// PS <player> <money> <armyValue> <units> <structures> <unitsKilled> <unitsLost> <buildingsKilled> <buildingsLost>
	//    <moneyEarned> <unitsBuilt>: every playable player's numbers every 2 s (3 Nov 2026, the user: observer tools and the
	//    after-action timeline). Army value = build cost of the player's living units.
	void publishPlayerStats(UnsignedInt frame)
	{
		if (!ThePlayerList || frame % (2 * LOGICFRAMES_PER_SECOND) != 0)
			return;
		Int army[MAX_PLAYER_COUNT] = {}, units[MAX_PLAYER_COUNT] = {}, structures[MAX_PLAYER_COUNT] = {};
		for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			Player *owner = obj->getControllingPlayer();
			if (!owner || obj->isEffectivelyDead() || owner->getPlayerIndex() < 0 || owner->getPlayerIndex() >= MAX_PLAYER_COUNT)
				continue;
			const Int i = owner->getPlayerIndex();
			if (obj->isKindOf(KINDOF_STRUCTURE))
				++structures[i];
			else if (obj->isKindOf(KINDOF_INFANTRY) || obj->isKindOf(KINDOF_VEHICLE) || obj->isKindOf(KINDOF_AIRCRAFT))
			{
				++units[i];
				army[i] += obj->getTemplate()->calcCostToBuild(owner);
			}
		}
		char line[256];
		for (Int i = 0; i < ThePlayerList->getPlayerCount() && i < MAX_PLAYER_COUNT; ++i)
		{
			Player *p = ThePlayerList->getNthPlayer(i);
			if (!p || !p->isPlayableSide())
				continue;
			ScoreKeeper *score = p->getScoreKeeper();
			snprintf(line, sizeof(line), "PS %d %u %d %d %d %d %d %d %d %d %d\n", p->getPlayerIndex(), p->getMoney()->countMoney(), army[i], units[i],
				structures[i], score->getTotalUnitsDestroyed(), score->getTotalUnitsLost(), score->getTotalBuildingsDestroyed(),
				score->getTotalBuildingsLost(), score->getTotalMoneyEarned(), score->getTotalUnitsBuilt());
			s_buffer += line;
		}
	}

	void publishResult()
	{
		Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		if (s_resultSent || !local || !TheVictoryConditions || !local->isPlayableSide())
			return;
		if (TheVictoryConditions->hasAchievedVictory(local))
			s_resultSent = 1;
		else if (TheVictoryConditions->hasBeenDefeated(local))
			s_resultSent = 2;
		if (!s_resultSent)
			return;
		s_buffer += s_resultSent == 1 ? "G victory\n" : "G defeat\n";
		ScoreKeeper *score = local->getScoreKeeper();
		char line[128];
		snprintf(line, sizeof(line), "GS %d %d %d %d %u\n", score->getTotalUnitsDestroyed(), score->getTotalBuildingsDestroyed(),
			score->getTotalUnitsBuilt(), score->getTotalMoneyEarned(), (UnsignedInt)(TheGameLogic->getFrame() / LOGICFRAMES_PER_SECOND));
		s_buffer += line;
	}

	// V <cellsX> <cellsY> <cellSize> <originX> <originY> <runs>: the local player's fog of war, partition cells row
	// by row, run-length coded as <c|f|s><count> (clear / fogged = seen before / shrouded = never seen), when it changes.
	void publishVision(UnsignedInt frame)
	{
		Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		const bool playback = TheRecorder && TheRecorder->isPlaybackMode();
		if (s_visionDirty)
		{
			s_visionDirty = false;
			s_lastVision.clear();
		}
		if (playback)   // the replay viewer's choice (viewplayer): a player's vision, or none (all clear)
			local = s_viewPlayer >= 0 && ThePlayerList ? ThePlayerList->getNthPlayer(s_viewPlayer) : nullptr;
		if (playback && !local && ThePartitionManager && TheTerrainLogic && (frame % 6) == 0 && s_lastVision != "all")
		{
			const Int cells = ThePartitionManager->getCellCountX() * ThePartitionManager->getCellCountY();
			Region3D extent;
			TheTerrainLogic->getExtent(&extent);
			char header[160];
			snprintf(header, sizeof(header), "V %d %d %.2f %.2f %.2f c%d\n", ThePartitionManager->getCellCountX(), ThePartitionManager->getCellCountY(),
				ThePartitionManager->getCellSize(), extent.lo.x, extent.lo.y, cells);
			s_buffer += header;
			s_lastVision = "all";
			return;
		}
		if (!local || !ThePartitionManager || !TheTerrainLogic || (frame % 6) != 0 || !local->isPlayableSide())
			return;
		const Int cellsX = ThePartitionManager->getCellCountX();
		const Int cellsY = ThePartitionManager->getCellCountY();
		std::string runs;
		runs.reserve(4096);
		char current = 0;
		Int count = 0;
		char number[16];
		for (Int y = 0; y < cellsY; ++y)
			for (Int x = 0; x < cellsX; ++x)
			{
				const CellShroudStatus status = ThePartitionManager->getShroudStatusForPlayer(local->getPlayerIndex(), x, y);
				const char code = status == CELLSHROUD_CLEAR ? 'c' : status == CELLSHROUD_FOGGED ? 'f' : 's';
				if (code != current && count)
				{
					snprintf(number, sizeof(number), "%c%d", current, count);
					runs += number;
					count = 0;
				}
				current = code;
				++count;
			}
		if (count)
		{
			snprintf(number, sizeof(number), "%c%d", current, count);
			runs += number;
		}
		if (runs == s_lastVision)
			return;
		s_lastVision = runs;
		Region3D extent;
		TheTerrainLogic->getExtent(&extent);
		char header[128];
		snprintf(header, sizeof(header), "V %d %d %.2f %.2f %.2f ", cellsX, cellsY, ThePartitionManager->getCellSize(), extent.lo.x, extent.lo.y);
		s_buffer += header;
		s_buffer += runs;
		s_buffer += "\n";
	}

	// L <localPlayerIndex> once; M <money> <powerProduced> <powerConsumed> when they change.
	void publishLocalPlayer()
	{
		Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
		if (!local)
			return;
		char line[96];
		if (local->getPlayerIndex() != s_localSent)
		{
			s_localSent = local->getPlayerIndex();
			snprintf(line, sizeof(line), "L %d\n", s_localSent);
			s_buffer += line;
		}
		const Int money = (Int)local->getMoney()->countMoney();
		const Int made = local->getEnergy()->getProduction();
		const Int used = local->getEnergy()->getConsumption();
		if (money != s_lastMoney || made != s_lastPowerMade || used != s_lastPowerUsed)
		{
			s_lastMoney = money;
			s_lastPowerMade = made;
			s_lastPowerUsed = used;
			snprintf(line, sizeof(line), "M %d %d %d\n", money, made, used);
			s_buffer += line;
		}
	}
	bool flush()
	{
		const char *data = s_buffer.data();
		int left = (int)s_buffer.size();
		while (left > 0)
		{
			const int sent = send(s_client, data, left, 0);
			if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
			{
				fd_set writable;
				FD_ZERO(&writable);
				FD_SET(s_client, &writable);
				timeval wait = { 1, 0 };
				select(0, nullptr, &writable, nullptr, &wait);
				continue;
			}
			if (sent <= 0)
				return false;
			data += sent;
			left -= sent;
		}
		s_buffer.clear();
		return true;
	}
}

// Skirmish ceasefire (2 Nov 2026, the user: a ceasefire time in the match setup): every pair of opposing players is made
// neutral when the battle starts (no unit attacks a neutral) and enemies again when the time is up; allies stay allies.
// Single-player skirmish only (the relations change outside the message stream). CF <seconds left> once a second, CF 0 at
// the end, for the client's banner.
// 2 Nov (play test): the time runs in real seconds whatever the game speed (frames / (30 x speed); a pause stops it), and
// no unit may enter an opposing base while it lasts ("no rush"): the AI's attack waves walked into the player's base and
// stood there, neutral, waiting. A unit that comes within NoRushRadius of a structure of a player it has a ceasefire with
// is sent back to its own base; the AI's turned-back units hunt when the ceasefire ends.
static void publishCeasefire(UnsignedInt frame)
{
	static Int s_state = 0; // 0 not started, 1 running, 2 over
	static std::vector<std::pair<Int, Int>> s_pairs;
	static double s_left = 0.0;
	static UnsignedInt s_lastFrame = 0;
	static Int s_lastShown = -1;
	static std::unordered_set<UnsignedInt> s_turnedBack;
	const Int seconds = TheGlobalData->m_ceasefireSeconds;
	if (seconds <= 0 || s_state == 2 || TheNetwork || !ThePlayerList || !TheGameLogic || (TheRecorder && TheRecorder->isPlaybackMode()))
		return;
	if (s_state == 0)
	{
		s_state = 1;
		s_left = seconds;
		s_lastFrame = frame;
		for (Int a = 0; a < ThePlayerList->getPlayerCount(); ++a)
			for (Int b = 0; b < ThePlayerList->getPlayerCount(); ++b)
			{
				Player *pa = ThePlayerList->getNthPlayer(a), *pb = ThePlayerList->getNthPlayer(b);
				if (a == b || !pa || !pb || !pa->isPlayableSide() || !pb->isPlayableSide() || !pb->getDefaultTeam())
					continue;
				if (pa->getRelationship(pb->getDefaultTeam()) == ENEMIES)
				{
					s_pairs.push_back(std::make_pair(a, b));
					pa->setPlayerRelationship(pb, NEUTRAL);
				}
			}
		printf("StateStream: ceasefire for %d s (%d relations)\n", seconds, (Int)s_pairs.size());
		fflush(stdout);
	}
	const double speed = s_speed > 0.05f ? s_speed : 0.05;
	s_left -= double(frame - s_lastFrame) / (LOGICFRAMES_PER_SECOND * speed);
	s_lastFrame = frame;
	char line[64];
	if (s_left <= 0.0)
	{
		for (const std::pair<Int, Int> &p : s_pairs)
		{
			Player *pa = ThePlayerList->getNthPlayer(p.first), *pb = ThePlayerList->getNthPlayer(p.second);
			if (pa && pb)
				pa->setPlayerRelationship(pb, ENEMIES);
		}
		Int hunting = 0;
		for (UnsignedInt id : s_turnedBack)
		{
			Object *obj = TheGameLogic->findObjectByID((ObjectID)id);
			Player *owner = obj ? obj->getControllingPlayer() : NULL;
			if (obj && !obj->isEffectivelyDead() && obj->getAIUpdateInterface() && owner && owner->getPlayerType() == PLAYER_COMPUTER)
			{
				obj->getAIUpdateInterface()->aiHunt(CMD_FROM_AI);
				++hunting;
			}
		}
		s_turnedBack.clear();
		s_state = 2;
		s_buffer += "CF 0\n";
		printf("StateStream: ceasefire over at frame %u (%d turned-back AI units hunt)\n", frame, hunting);
		fflush(stdout);
		return;
	}
	const Int shown = (Int)ceil(s_left);
	if (shown != s_lastShown)
	{
		s_lastShown = shown;
		snprintf(line, sizeof(line), "CF %d\n", shown);
		s_buffer += line;
	}
	if (frame % 10 != 0)
		return;
	// No rush: each player's base is the ground within NoRushRadius of its structures.
	const Real NoRushRadius = 350.0f;
	const Int count = ThePlayerList->getPlayerCount();
	std::vector<std::vector<Coord3D>> bases(count);
	std::vector<Coord3D> homes(count);
	std::vector<char> hasHome(count, 0);
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		Player *owner = obj->getControllingPlayer();
		if (!owner || !obj->isKindOf(KINDOF_STRUCTURE) || obj->isEffectivelyDead())
			continue;
		const Int index = owner->getPlayerIndex();
		if (index < 0 || index >= count)
			continue;
		bases[index].push_back(*obj->getPosition());
		if (!hasHome[index] || obj->isKindOf(KINDOF_COMMANDCENTER))
		{
			homes[index] = *obj->getPosition();
			hasHome[index] = obj->isKindOf(KINDOF_COMMANDCENTER) ? 2 : 1;
		}
	}
	std::vector<std::vector<Int>> opponents(count);
	for (const std::pair<Int, Int> &p : s_pairs)
		if (p.first < count && p.second < count)
			opponents[p.first].push_back(p.second);
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		Player *owner = obj->getControllingPlayer();
		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if (!owner || !ai || obj->isEffectivelyDead() || obj->getContainedBy() || obj->isKindOf(KINDOF_STRUCTURE) ||
			obj->isKindOf(KINDOF_PROJECTILE) || obj->isKindOf(KINDOF_HARVESTER) || obj->isKindOf(KINDOF_DRONE) ||
			obj->isKindOf(KINDOF_IMMOBILE))
			continue;
		const Int index = owner->getPlayerIndex();
		if (index < 0 || index >= count || !hasHome[index] || opponents[index].empty())
			continue;
		const Coord3D *pos = obj->getPosition();
		bool inside = false;
		for (Int other : opponents[index])
		{
			for (const Coord3D &s : bases[other])
			{
				const Real dx = s.x - pos->x, dy = s.y - pos->y;
				if (dx * dx + dy * dy < NoRushRadius * NoRushRadius)
				{
					inside = true;
					break;
				}
			}
			if (inside)
				break;
		}
		if (!inside)
			continue;
		ai->aiMoveToPosition(&homes[index], owner->getPlayerType() == PLAYER_COMPUTER ? CMD_FROM_AI : CMD_FROM_PLAYER);
		if (s_turnedBack.insert((UnsignedInt)obj->getID()).second)
		{
			printf("StateStream: ceasefire turned back %s (player %d)\n", obj->getTemplate()->getName().str(), index);
			fflush(stdout);
		}
	}
}
void StateStream::publish(UnsignedInt frame, Object *firstObject){
	const Int port = TheGlobalData->m_streamStatePort;
	if (port <= 0 || s_failed)
		return;
	if (s_client == INVALID_SOCKET && !connectClient(port))
	{
		printf("StateStream: could not open port %d; streaming disabled\n", port);
		s_failed = true;
		return;
	}

	Int count = 0;
	for (Object *obj = firstObject; obj; obj = obj->getNextObject())
		++count;

	pollCommands();
	publishWater();
	publishBridges();
	publishPlayers();
	publishNetwork(frame);
	publishLocalPlayer();
	publishVision(frame);
	publishResult();
	publishPlayerStats(frame);
	ZHStreamScriptCounters(frame);
	publishCeasefire(frame);
	ZHEvaSuperweaponReady(frame);
	ZHAIPersonalities(frame);
	// The skirmish's game speed (-gamespeed) from its first frame; the battle menu changes it later (speed).
	static bool s_speedSet = false;
	if (!s_speedSet)
	{
		s_speedSet = true;
		// The engine's own 30 FPS limiter held a faster skirmish at 30 (the stream's pacing keeps real time instead).
		if (!TheNetwork && TheGlobalData->m_lanSlot < 0 && TheGlobalData->m_streamRealtime)
			TheWritableGlobalData->m_useFpsLimit = false;
		if (!TheNetwork && TheGlobalData->m_lanSlot < 0 && TheGlobalData->m_gameSpeed > 0.0f && !(TheRecorder && TheRecorder->isPlaybackMode()))
			s_speed = TheGlobalData->m_gameSpeed < 0.25f ? 0.25f : TheGlobalData->m_gameSpeed > 3.0f ? 3.0f : TheGlobalData->m_gameSpeed;
	}
	char line[512];
	snprintf(line, sizeof(line), "F %u %d\n", frame, count);
	s_buffer += line;
	s_buffer += s_moneyLines;
	s_moneyLines.clear();
	// RP <frame> <total frames> <speed>: replay progress for the client's timeline (playback only, once a second).
	if (TheRecorder && TheRecorder->isPlaybackMode() && frame % LOGICFRAMES_PER_SECOND == 0)
	{
		snprintf(line, sizeof(line), "RP %u %u %g\n", frame, TheRecorder->getPlaybackFrameCount(), s_speed);
		s_buffer += line;
	}

	std::unordered_set<UnsignedInt> seen;
	seen.reserve(count);
	for (Object *obj = firstObject; obj; obj = obj->getNextObject())
	{
		const UnsignedInt id = (UnsignedInt)obj->getID();
		seen.insert(id);
		if (s_known.insert(id).second)
		{
			const Player *owner = obj->getControllingPlayer();
			const ThingTemplate *thing = obj->getTemplate();
			snprintf(line, sizeof(line), "S %u %d %s\n", id, owner ? (Int)owner->getPlayerIndex() : -1,
				thing ? thing->getName().str() : "?");
			s_buffer += line;
			// CP <id> <flags> <max>: 1 capturable (tech buildings), 2 garrisonable, 4 a transport/container units can enter;
			// max = how many it holds (10 Nov 2026: the client's inventory panel shows that many slots, as the original's).
			{
				UnsignedInt flags = obj->isKindOf(KINDOF_CAPTURABLE) ? 1u : 0u;
				Int containMax = 0;
				if (ContainModuleInterface *contain = obj->getContain())
				{
					flags |= contain->isGarrisonable() ? 2u : (contain->getContainMax() > 0 ? 4u : 0u);
					containMax = contain->getContainMax();
				}
				if (flags)
				{
					snprintf(line, sizeof(line), "CP %u %u %d\n", id, flags, containMax);
					s_buffer += line;
				}
			}
		}
		const Coord3D *pos = obj->getPosition();
		const BodyModuleInterface *body = obj->getBodyModule();
		// Weapon-set bits (the model switches of upgrades and veterancy): 1 player upgrade (e.g. Humvee TOW), 2 crate
		// upgrade one, 4 crate upgrade two (Marauder turrets), 8 veteran, 16 elite, 32 heroic.
		const UnsignedInt weaponSets = (obj->testWeaponSetFlag(WEAPONSET_PLAYER_UPGRADE) ? 1u : 0u) | (obj->testWeaponSetFlag(WEAPONSET_CRATEUPGRADE_ONE) ? 2u : 0u)
			| (obj->testWeaponSetFlag(WEAPONSET_CRATEUPGRADE_TWO) ? 4u : 0u) | (obj->testWeaponSetFlag(WEAPONSET_VETERAN) ? 8u : 0u)
			| (obj->testWeaponSetFlag(WEAPONSET_ELITE) ? 16u : 0u) | (obj->testWeaponSetFlag(WEAPONSET_HERO) ? 32u : 0u);
		// Last field: height above the terrain (sim units), so the client knows flight altitude exactly (rotor downwash).
		snprintf(line, sizeof(line), "U %u %.2f %.2f %.2f %.4f %.1f %.1f %d %x %x %.1f\n", id, pos->x, pos->y, pos->z,
			obj->getOrientation(), body ? body->getHealth() : 0.0f, body ? body->getMaxHealth() : 0.0f,
			obj->isEffectivelyDead() ? 1 : 0, conditionBits(obj), weaponSets, obj->getHeightAboveTerrain());
		s_buffer += line;
		// OW <id> <owner>: the controlling player changed (a captured or garrisoned building, a hijacked vehicle);
		// CN <id> <container>: the object went into (container id) or out of (0) a building or transport (27 Oct).
		{
			const Player *now = obj->getControllingPlayer();
			const Int index = now ? (Int)now->getPlayerIndex() : -1;
			auto known = s_owners.find(id);
			if (known == s_owners.end())
				s_owners[id] = index;
			else if (known->second != index)
			{
				known->second = index;
				snprintf(line, sizeof(line), "OW %u %d\n", id, index);
				s_buffer += line;
			}
			const Object *container = obj->getContainedBy();
			const UnsignedInt inside = container ? (UnsignedInt)container->getID() : 0u;
			UnsignedInt &was = s_containers[id];
			if (was != inside)
			{
				was = inside;
				snprintf(line, sizeof(line), "CN %u %u\n", id, inside);
				s_buffer += line;
			}
			// EX <id> <door x y z> <natural rally x y z>: where a factory's units come out and the first point they drive to,
			// once per producer (28 Oct 2026, the user: tanks left a raised war factory beside its ramp - the client put the
			// ramp on the model's front, the door is elsewhere on many of them).
			if (ExitInterface *exit = s_exitSent.count(id) ? nullptr : const_cast<Object *>(obj)->getObjectExitInterface())
			{
				Coord3D door, rally;
				if (exit->getExitPosition(door) && exit->getNaturalRallyPoint(rally, FALSE))
				{
					snprintf(line, sizeof(line), "EX %u %.1f %.1f %.1f %.1f %.1f %.1f\n", id, door.x, door.y, door.z, rally.x, rally.y, rally.z);
					s_buffer += line;
				}
				s_exitSent.insert(id);
			}
			// MT <id> <target x y z>: a guided missile's destination, once it is launched (30 Oct 2026, the user: the nuke and
			// the SCUDs should fly an arc - the client draws a ballistic arc from the launch to this point; the original's
			// straight climb, hard turn and straight dive read as lines from the RTS camera).
			if (!s_missileSent.count(id))
			{
				static const NameKeyType key_Neutron = NAMEKEY("NeutronMissileUpdate");
				static const NameKeyType key_MissileAI = NAMEKEY("MissileAIUpdate");
				const Coord3D *target = nullptr;
				if (const NeutronMissileUpdate *neutron = (const NeutronMissileUpdate *)obj->findUpdateModule(key_Neutron))
					target = &neutron->getTargetPosForStream();
				else if (const MissileAIUpdate *missile = (const MissileAIUpdate *)obj->findUpdateModule(key_MissileAI))
					target = &missile->getTargetPosForStream();
				if (target && (target->x != 0.0f || target->y != 0.0f))
				{
					snprintf(line, sizeof(line), "MT %u %.1f %.1f %.1f\n", id, target->x, target->y, target->z);
					s_buffer += line;
					s_missileSent.insert(id);
				}
			}
		}
		// FL <unitId> <targetId> <percent>: a unit capturing a building (RAISING_FLAG), with its progress, every half second
		// (28 Oct, the user: a flag in the team colour rises while the building is taken).
		if (frame % 15 == 0 && (conditionBits(obj) & (1u << 30)))
		{
			for (BehaviorModule **module = obj->getBehaviorModules(); *module; ++module)
			{
				SpecialPowerUpdateInterface *power = (*module)->getSpecialPowerUpdateInterface();
				if (power && power->isSpecialAbility())
				{
					const SpecialAbilityUpdate *ability = static_cast<SpecialAbilityUpdate *>(power);
					snprintf(line, sizeof(line), "FL %u %u %d\n", id, (unsigned)ability->getAbilityTargetID(), (Int)(ability->getPreparationProgress() * 100.0f));
					s_buffer += line;
					break;
				}
			}
		}

		// Local player only: production queues and construction progress, when they change.
		const Player *owner = obj->getControllingPlayer();
		if (owner && ThePlayerList && owner == ThePlayerList->getLocalPlayer())
		{
			if (ProductionUpdateInterface *production = obj->getProductionUpdateInterface())
			{
				// Q <factory> <count> <template>:<percent>... (upgrades are written as upgrade name)
				std::string queue;
				Int entries = 0;
				for (const ProductionEntry *entry = production->firstProduction(); entry; entry = production->nextProduction(entry))
				{
					const char *what = entry->getProductionType() == PRODUCTION_UNIT && entry->getProductionObject() ? entry->getProductionObject()->getName().str()
						: entry->getProductionUpgrade() ? entry->getProductionUpgrade()->getUpgradeName().str() : "?";
					snprintf(line, sizeof(line), " %s:%d", what, (Int)min(100.0f, entry->getPercentComplete())); // fast build counts past 100 while the unit waits for the door
					queue += line;
					++entries;
				}
				std::string &last = s_queues[id];
				if (queue != last)
				{
					last = queue;
					snprintf(line, sizeof(line), "Q %u %d", id, entries);
					s_buffer += line;
					s_buffer += queue;
					s_buffer += "\n";
				}
			}
			// A sold building counts its construction back down to 0 (BuildAssistant, 3 s after 1.5 s): streamed too, so the
			// client takes it apart at once instead of nothing happening until it vanishes (28 Oct, the user: selling is slow).
			if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) || obj->testStatus(OBJECT_STATUS_SOLD))
			{
				// C <id> <percent>: building under construction (or being sold)
				const Int percent = max(0, (Int)obj->getConstructionPercent());
				auto known = s_construction.find(id);
				if (known == s_construction.end() || known->second != percent)
				{
					s_construction[id] = percent;
					snprintf(line, sizeof(line), "C %u %d\n", id, percent);
					s_buffer += line;
				}
			}
			else if (s_construction.erase(id))
			{
				snprintf(line, sizeof(line), "C %u 100\n", id);
				s_buffer += line;
			}
		}

		// T <id> <turret> <angle> <pitch>: turret angles relative to the body, radians, when they change.
		if (const AIUpdateInterface *ai = obj->getAI())
		{
			for (Int turret = 0; turret < MAX_TURRETS; ++turret)
			{
				Real angle = 0.0f, pitch = 0.0f;
				if (!ai->getTurretRotAndPitch((WhichTurretType)turret, &angle, &pitch))
					continue;
				auto known = s_turrets.find(id);
				if (known == s_turrets.end())
				{
					known = s_turrets.emplace(id, TurretState()).first;
					for (Int k = 0; k < MAX_TURRETS; ++k)
						known->second.angle[k] = known->second.pitch[k] = 1000.0f;
				}
				TurretState &last = known->second;
				if (fabsf(last.angle[turret] - angle) < 0.001f && fabsf(last.pitch[turret] - pitch) < 0.001f)
					continue;
				last.angle[turret] = angle;
				last.pitch[turret] = pitch;
				snprintf(line, sizeof(line), "T %u %d %.4f %.4f\n", id, turret, angle, pitch);
				s_buffer += line;
			}
		}

		// O <id> <boxes>: supply left in a dock or pile (the client empties the crate stacks as it drops), when it changes.
		if (obj->isKindOf(KINDOF_SUPPLY_SOURCE))
		{
			static const NameKeyType key_SupplyWarehouseDockUpdate = NAMEKEY("SupplyWarehouseDockUpdate");
			if (const SupplyWarehouseDockUpdate *dock = (const SupplyWarehouseDockUpdate *)obj->findUpdateModule(key_SupplyWarehouseDockUpdate))
			{
				const Int boxes = dock->getBoxesStored();
				auto known = s_supply.find(id);
				if (known == s_supply.end() || known->second != boxes)
				{
					s_supply[id] = boxes;
					snprintf(line, sizeof(line), "O %u %d\n", id, boxes);
					s_buffer += line;
				}
			}
		}

		// Z <id> <sx> <sy> <sz> <ex> <ey> <ez> <widthScale>: laser beam ends (W3DLaserDraw objects: lasers, data and
		// disable streams, the particle uplink cannon), from the drawable's LaserUpdate, when they move or the width changes.
		if (Drawable *draw = obj->getDrawable())
		{
			static const NameKeyType key_LaserUpdate = NAMEKEY("LaserUpdate");
			if (const LaserUpdate *laser = (const LaserUpdate *)draw->findClientUpdateModule(key_LaserUpdate))
			{
				const Coord3D *a = laser->getStartPos(), *b = laser->getEndPos();
				const Real scale = laser->getWidthScale();
				LaserState &last = s_lasers[id];
				const Real moved = fabsf(last.start.x - a->x) + fabsf(last.start.y - a->y) + fabsf(last.start.z - a->z)
					+ fabsf(last.end.x - b->x) + fabsf(last.end.y - b->y) + fabsf(last.end.z - b->z);
				if (moved > 0.5f || fabsf(last.scale - scale) > 0.02f)
				{
					last.start = *a;
					last.end = *b;
					last.scale = scale;
					snprintf(line, sizeof(line), "Z %u %.1f %.1f %.1f %.1f %.1f %.1f %.3f\n", id, a->x, a->y, a->z, b->x, b->y, b->z, scale);
					s_buffer += line;
				}
			}
		}
	}
	for (auto it = s_known.begin(); it != s_known.end();)
	{
		if (seen.count(*it) == 0)
		{
			snprintf(line, sizeof(line), "D %u\n", *it);
			s_buffer += line;
			s_turrets.erase(*it);
			s_lasers.erase(*it);
			s_queues.erase(*it);
			s_construction.erase(*it);
			s_owners.erase(*it);
			s_containers.erase(*it);
			s_supply.erase(*it);
			it = s_known.erase(it);
		}
		else
			++it;
	}
	// Y <drawableId> <template> <sx> <sy> <sz> <ex> <ey> <ez> <widthScale>: laser beams that are drawables without an
	// object (the particle uplink cannon's connector and orbital lasers); YD <drawableId> when one is gone.
	if (TheGameClient)
	{
		static const NameKeyType key_LaserUpdate = NAMEKEY("LaserUpdate");
		std::unordered_set<UnsignedInt> live;
		for (Drawable *draw = TheGameClient->firstDrawable(); draw; draw = draw->getNextDrawable())
		{
			if (draw->getObject())
				continue;
			const LaserUpdate *laser = (const LaserUpdate *)draw->findClientUpdateModule(key_LaserUpdate);
			if (!laser)
				continue;
			const UnsignedInt did = (UnsignedInt)draw->getID();
			live.insert(did);
			const Coord3D *a = laser->getStartPos(), *b = laser->getEndPos();
			const Real scale = laser->getWidthScale();
			LaserState &last = s_drawLasers[did];
			const Real moved = fabsf(last.start.x - a->x) + fabsf(last.start.y - a->y) + fabsf(last.start.z - a->z)
				+ fabsf(last.end.x - b->x) + fabsf(last.end.y - b->y) + fabsf(last.end.z - b->z);
			if (moved > 0.5f || fabsf(last.scale - scale) > 0.02f)
			{
				last.start = *a;
				last.end = *b;
				last.scale = scale;
				snprintf(line, sizeof(line), "Y %u %s %.1f %.1f %.1f %.1f %.1f %.1f %.3f\n", did,
					draw->getTemplate() ? draw->getTemplate()->getName().str() : "?", a->x, a->y, a->z, b->x, b->y, b->z, scale);
				s_buffer += line;
			}
		}
		for (auto it = s_drawLasers.begin(); it != s_drawLasers.end();)
		{
			if (live.count(it->first) == 0)
			{
				snprintf(line, sizeof(line), "YD %u\n", it->first);
				s_buffer += line;
				it = s_drawLasers.erase(it);
			}
			else
				++it;
		}
	}
	flushAudioEnds();
	s_buffer += "E\n";

	if (!flush())
	{
		printf("StateStream: client disconnected; streaming stopped\n");
		closesocket(s_client);
		s_client = INVALID_SOCKET;
		s_failed = true;
		return;
	}

	// Hold after the first frame until the client's loading screen is done (ready), at most 4 minutes.
	if (s_waitReady && ++s_readyFrames >= 2)
	{
		// The bridges the logic knows (27 Oct, the user: units could not cross the bridges).
		int bridges = 0;
		for (Bridge *bridge = TheTerrainLogic ? TheTerrainLogic->getFirstBridge() : nullptr; bridge; bridge = bridge->getNext())
		{
			const BridgeInfo *info = bridge->peekBridgeInfo();
			printf("StateStream: bridge %d (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) width %.0f\n", ++bridges, info->from.x, info->from.y, info->from.z,
				info->to.x, info->to.y, info->to.z, info->bridgeWidth);
		}
		printf("StateStream: %d bridges in the logic\n", bridges);
		fflush(stdout);
		static const bool noWait = getenv("GENZH_NO_READY_WAIT") != nullptr;
		const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(4);
		while (!noWait && s_waitReady && s_client != INVALID_SOCKET && std::chrono::steady_clock::now() < until)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			pollCommands();
		}
		s_waitReady = false;
		s_nextFrame = std::chrono::steady_clock::now();
	}
	if (s_stepMode)
	{
		while (s_stepBudget <= 0 && s_client != INVALID_SOCKET)
		{
			std::this_thread::sleep_for(std::chrono::microseconds(500));
			pollCommands();
		}
		--s_stepBudget;
		return;
	}
	// -streamrealtime: pace the headless simulation at game speed so a viewer can watch it.
	if (TheGlobalData->m_streamRealtime)
	{
		// Paused: hold this frame, keep reading commands (speed) and keep the socket alive.
		while (s_speed <= 0.0f && s_client != INVALID_SOCKET)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			pollCommands();
			s_nextFrame = std::chrono::steady_clock::now();
		}
		s_nextFrame += std::chrono::microseconds(int(1000000 / (LOGICFRAMES_PER_SECOND * (s_speed > 0.0f ? s_speed : 1.0f))));
		const auto now = std::chrono::steady_clock::now();
		if (s_nextFrame < now - std::chrono::milliseconds(250))
			s_nextFrame = now; // after a hitch or a speed change: no catch-up burst
		std::this_thread::sleep_until(s_nextFrame);
	}
}

void StateStream::shutdown()
{
	if (s_client != INVALID_SOCKET)
	{
		::shutdown(s_client, 1 /* SD_SEND */);
		closesocket(s_client);
		s_client = INVALID_SOCKET;
	}
}

// ------------------------------------------------------------------------------------------------ campaign presentation
// ZH2026 campaign (3 Nov 2026, the user: port the original campaign): the headless sim's tactical view and in-game UI are
// dummies, so the script actions that present a mission (text, briefings, the camera, letterbox, input lock, movies,
// counters, fades) go to the client as SX records, queued with the next frame:
//   SX TX <text>              DISPLAY_TEXT (message area)          SX MC <ms> <text>   SHOW_MILITARY_CAPTION (briefing)
//   SX CT <s> <text>          DISPLAY_CINEMATIC_TEXT (letterbox)   SX POP <text>       INGAME_POPUP_MESSAGE
//   SX SUB <text>             subtitle of a SPEECH_PLAY (DIALOGEVENT:<event>Subtitle)
//   SX LB 0|1  SX IN 0|1      letterbox, player input (0 = locked)  SX MV|MR <movie>    full screen / radar movie
//   SX FADE <add|sub|sat|mul> <min> <max> <in> <hold> <out>  (frames)  SX BW 0|1 <frames>   black and white view
//   SX SHAKE <0..5>           SX MUSIC <track>
//   SX CAM MOVE <x y z> <ms> | PATH <ms> <n> <x y z>... | ZOOM <z> <ms> | PITCH <p> <ms> | ROT <turns> <ms>
//          LOOK <x y z> | FLOOK <x y z> | RESET <x y z> <ms> | SETUP <x y z> <zoom> <pitch> <lx ly lz> | FOLLOW <id> <snap>
//          UNFOLLOW | HOME | FACE <x y z> <ms> | DEFAULT <pitch> <angle> <zoom> | FZOOM <z> <ms> | FPITCH <p> <ms>
//   SX TM <name> <0 counter|1 countdown> <text> | SX TMHIDE <name> | SX TV <name> <value>   (value: once a second)
//   G victory|defeat          the mission's own VICTORY / QUICKVICTORY / DEFEAT / LOCALDEFEAT
// A scripted camera move keeps CAMERA_MOVEMENT_FINISHED false for its duration (ScriptConditions.cpp).
UnsignedInt TheZHCameraBusyUntil = 0;
namespace
{
	std::unordered_map<std::string, int> s_shownCounters; // name -> 1 countdown, 0 counter

	std::string Utf8Line(const UnicodeString &text)
	{
		const WideChar *w = text.str();
		const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
		std::string raw(n > 1 ? n - 1 : 0, '\0');
		if (n > 1)
			WideCharToMultiByte(CP_UTF8, 0, w, -1, &raw[0], n, nullptr, nullptr);
		std::string out;
		for (char c : raw)
		{
			if (c == '\n')
				out += "\\n";
			else if (c != '\r')
				out += c;
		}
		return out;
	}
	std::string Label(const AsciiString &label) { return Utf8Line(TheGameText ? TheGameText->fetch(label) : UnicodeString(L"")); }
	bool WaypointAt(const AsciiString &name, Coord3D &out)
	{
		Waypoint *way = TheTerrainLogic ? TheTerrainLogic->getWaypointByName(name) : nullptr;
		if (!way)
			return false;
		out = *way->getLocation();
		return true;
	}
	void CameraBusy(Real seconds)
	{
		const UnsignedInt end = TheGameLogic->getFrame() + (UnsignedInt)(seconds * LOGICFRAMES_PER_SECOND + 0.5f);
		if (end > TheZHCameraBusyUntil)
			TheZHCameraBusyUntil = end;
	}
}

void ZHStreamScriptAction(ScriptAction *a)
{
	if (!a || !TheGlobalData || TheGlobalData->m_streamStatePort <= 0 || !TheGameLogic)
		return;
	auto Str = [a](Int i) { Parameter *p = i < a->getNumParameters() ? a->getParameter(i) : nullptr; return p ? p->getString() : AsciiString(); };
	auto Num = [a](Int i) { Parameter *p = i < a->getNumParameters() ? a->getParameter(i) : nullptr; return p ? p->getReal() : 0.0f; };
	auto Int_ = [a](Int i) { Parameter *p = i < a->getNumParameters() ? a->getParameter(i) : nullptr; return p ? p->getInt() : 0; };
	char line[512];
	std::string out;
	Coord3D c, d;
	switch (a->getActionType())
	{
	case ScriptAction::DISPLAY_TEXT: out = "SX TX " + Label(Str(0)); break;
	case ScriptAction::SHOW_MILITARY_CAPTION: snprintf(line, sizeof(line), "SX MC %d ", Int_(1)); out = line + Label(Str(0)); break;
	case ScriptAction::DISPLAY_CINEMATIC_TEXT: snprintf(line, sizeof(line), "SX CT %d ", Int_(2)); out = line + Label(Str(0)); break;
	case ScriptAction::INGAME_POPUP_MESSAGE: out = "SX POP " + Label(Str(0)); break;
	case ScriptAction::SPEECH_PLAY:
	{
		AsciiString key;
		key.format("DIALOGEVENT:%sSubtitle", Str(0).str());
		const UnicodeString sub = TheGameText ? TheGameText->fetch(key) : UnicodeString(L"");
		const std::string text = Utf8Line(sub);
		if (!text.empty() && text[0] != '*' && text.find("MISSING") == std::string::npos)
			out = "SX SUB " + text;
		break;
	}
	case ScriptAction::CAMERA_LETTERBOX_BEGIN: out = "SX LB 1"; break;
	case ScriptAction::CAMERA_LETTERBOX_END: out = "SX LB 0"; break;
	case ScriptAction::DISABLE_INPUT: out = "SX IN 0"; break;
	case ScriptAction::ENABLE_INPUT: out = "SX IN 1"; break;
	case ScriptAction::MOVIE_PLAY_FULLSCREEN: out = std::string("SX MV ") + Str(0).str(); break;
	case ScriptAction::MOVIE_PLAY_RADAR: out = std::string("SX MR ") + Str(0).str(); break;
	case ScriptAction::MUSIC_SET_TRACK: out = std::string("SX MUSIC ") + Str(0).str(); break;
	case ScriptAction::SCREEN_SHAKE: snprintf(line, sizeof(line), "SX SHAKE %d", Int_(0)); out = line; break;
	case ScriptAction::CAMERA_BW_MODE_BEGIN: snprintf(line, sizeof(line), "SX BW 1 %d", Int_(0)); out = line; break;
	case ScriptAction::CAMERA_BW_MODE_END: snprintf(line, sizeof(line), "SX BW 0 %d", Int_(0)); out = line; break;
	case ScriptAction::CAMERA_FADE_ADD: case ScriptAction::CAMERA_FADE_SUBTRACT: case ScriptAction::CAMERA_FADE_SATURATE: case ScriptAction::CAMERA_FADE_MULTIPLY:
	{
		const char *kind = a->getActionType() == ScriptAction::CAMERA_FADE_ADD ? "add" : a->getActionType() == ScriptAction::CAMERA_FADE_SUBTRACT ? "sub"
			: a->getActionType() == ScriptAction::CAMERA_FADE_SATURATE ? "sat" : "mul";
		snprintf(line, sizeof(line), "SX FADE %s %g %g %d %d %d", kind, Num(0), Num(1), Int_(2), Int_(3), Int_(4));
		out = line;
		break;
	}
	case ScriptAction::MOVE_CAMERA_TO:
		if (WaypointAt(Str(0), c))
		{
			snprintf(line, sizeof(line), "SX CAM MOVE %.1f %.1f %.1f %d", c.x, c.y, c.z, (Int)(Num(1) * 1000.0f));
			out = line;
			CameraBusy(Num(1));
		}
		break;
	case ScriptAction::MOVE_CAMERA_ALONG_WAYPOINT_PATH:
	{
		Waypoint *way = TheTerrainLogic ? TheTerrainLogic->getWaypointByName(Str(0)) : nullptr;
		std::string points;
		Int n = 0;
		for (; way && n < 40; ++n)
		{
			snprintf(line, sizeof(line), " %.1f %.1f %.1f", way->getLocation()->x, way->getLocation()->y, way->getLocation()->z);
			points += line;
			way = way->getNumLinks() > 0 ? way->getLink(0) : nullptr;
		}
		if (n)
		{
			snprintf(line, sizeof(line), "SX CAM PATH %d %d", (Int)(Num(1) * 1000.0f), n);
			out = line + points;
			CameraBusy(Num(1));
		}
		break;
	}
	case ScriptAction::ZOOM_CAMERA: snprintf(line, sizeof(line), "SX CAM ZOOM %g %d", Num(0), (Int)(Num(1) * 1000.0f)); out = line; CameraBusy(Num(1)); break;
	case ScriptAction::PITCH_CAMERA: snprintf(line, sizeof(line), "SX CAM PITCH %g %d", Num(0), (Int)(Num(1) * 1000.0f)); out = line; CameraBusy(Num(1)); break;
	case ScriptAction::ROTATE_CAMERA: snprintf(line, sizeof(line), "SX CAM ROT %g %d", Num(0), (Int)(Num(1) * 1000.0f)); out = line; CameraBusy(Num(1)); break;
	case ScriptAction::CAMERA_MOD_SET_FINAL_ZOOM: snprintf(line, sizeof(line), "SX CAM FZOOM %g %d", Num(0), 0); out = line; break;
	case ScriptAction::CAMERA_MOD_SET_FINAL_PITCH: snprintf(line, sizeof(line), "SX CAM FPITCH %g %d", Num(0), 0); out = line; break;
	case ScriptAction::CAMERA_MOD_LOOK_TOWARD: case ScriptAction::CAMERA_MOD_FINAL_LOOK_TOWARD:
		if (WaypointAt(Str(0), c))
		{
			snprintf(line, sizeof(line), "SX CAM %s %.1f %.1f %.1f", a->getActionType() == ScriptAction::CAMERA_MOD_LOOK_TOWARD ? "LOOK" : "FLOOK", c.x, c.y, c.z);
			out = line;
		}
		break;
	case ScriptAction::CAMERA_LOOK_TOWARD_WAYPOINT:
		if (WaypointAt(Str(0), c))
		{
			snprintf(line, sizeof(line), "SX CAM FACE %.1f %.1f %.1f %d", c.x, c.y, c.z, (Int)(Num(1) * 1000.0f));
			out = line;
			CameraBusy(Num(1));
		}
		break;
	case ScriptAction::CAMERA_LOOK_TOWARD_OBJECT:
		if (const Object *unit = TheScriptEngine->getUnitNamed(Str(0)))
		{
			snprintf(line, sizeof(line), "SX CAM FACE %.1f %.1f %.1f %d", unit->getPosition()->x, unit->getPosition()->y, unit->getPosition()->z, (Int)(Num(1) * 1000.0f));
			out = line;
			CameraBusy(Num(1) + Num(2));
		}
		break;
	case ScriptAction::RESET_CAMERA:
		if (WaypointAt(Str(0), c))
		{
			snprintf(line, sizeof(line), "SX CAM RESET %.1f %.1f %.1f %d", c.x, c.y, c.z, (Int)(Num(1) * 1000.0f));
			out = line;
			CameraBusy(Num(1));
		}
		break;
	case ScriptAction::SETUP_CAMERA:
		if (WaypointAt(Str(0), c) && WaypointAt(Str(3), d))
		{
			snprintf(line, sizeof(line), "SX CAM SETUP %.1f %.1f %.1f %g %g %.1f %.1f %.1f", c.x, c.y, c.z, Num(1), Num(2), d.x, d.y, d.z);
			out = line;
		}
		break;
	case ScriptAction::CAMERA_FOLLOW_NAMED: case ScriptAction::CAMERA_TETHER_NAMED:
		if (const Object *unit = TheScriptEngine->getUnitNamed(Str(0)))
		{
			snprintf(line, sizeof(line), "SX CAM FOLLOW %u %d", (unsigned)unit->getID(), Int_(1));
			out = line;
		}
		break;
	case ScriptAction::CAMERA_STOP_FOLLOW: case ScriptAction::CAMERA_STOP_TETHER_NAMED: out = "SX CAM UNFOLLOW"; break;
	case ScriptAction::CAMERA_MOVE_HOME: out = "SX CAM HOME"; break;
	case ScriptAction::CAMERA_SET_DEFAULT: snprintf(line, sizeof(line), "SX CAM DEFAULT %g %g %g", Num(0), Num(1), Num(2)); out = line; break;
	case ScriptAction::DISPLAY_COUNTDOWN_TIMER: case ScriptAction::DISPLAY_COUNTER:
	{
		const int countdown = a->getActionType() == ScriptAction::DISPLAY_COUNTDOWN_TIMER ? 1 : 0;
		s_shownCounters[Str(0).str()] = countdown;
		snprintf(line, sizeof(line), "SX TM %s %d ", Str(0).str(), countdown);
		out = line + Label(Str(1));
		break;
	}
	case ScriptAction::HIDE_COUNTDOWN_TIMER: case ScriptAction::HIDE_COUNTER:
		s_shownCounters.erase(Str(0).str());
		out = std::string("SX TMHIDE ") + Str(0).str();
		break;
	case ScriptAction::VICTORY: case ScriptAction::QUICKVICTORY: case ScriptAction::DEFEAT: case ScriptAction::LOCALDEFEAT:
		if (!s_resultSent)
		{
			const bool won = a->getActionType() == ScriptAction::VICTORY || a->getActionType() == ScriptAction::QUICKVICTORY;
			s_resultSent = won ? 1 : 2;
			out = won ? "G victory" : "G defeat";
			if (Player *local = ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr)
			{
				ScoreKeeper *score = local->getScoreKeeper();
				snprintf(line, sizeof(line), "\nGS %d %d %d %d %u", score->getTotalUnitsDestroyed(), score->getTotalBuildingsDestroyed(),
					score->getTotalUnitsBuilt(), score->getTotalMoneyEarned(), (UnsignedInt)(TheGameLogic->getFrame() / LOGICFRAMES_PER_SECOND));
				out += line;
			}
		}
		break;
	default: break;
	}
	if (out.empty())
		return;
	s_moneyLines += out;
	s_moneyLines += "\n";
}

// The values of the counters and countdown timers a mission shows (SX TV <name> <value>: a countdown in seconds), once a
// second (called from publish).
void ZHStreamScriptCounters(UnsignedInt frame)
{
	if (s_shownCounters.empty() || frame % LOGICFRAMES_PER_SECOND != 0 || !TheScriptEngine)
		return;
	char line[160];
	for (const auto &shown : s_shownCounters)
	{
		const TCounter *counter = TheScriptEngine->getCounter(AsciiString(shown.first.c_str()));
		if (!counter)
			continue;
		const Int value = shown.second ? counter->value / LOGICFRAMES_PER_SECOND : counter->value;
		snprintf(line, sizeof(line), "SX TV %s %d\n", shown.first.c_str(), value);
		s_moneyLines += line;
	}
}

// Superweapon ready (4 Nov 2026, plan 2.5): the original announces it from InGameUI's timer drawing, which a headless sim
// never runs - nobody ever heard "superweapon ready". The same rule here, twice a second: a finished superweapon whose
// power turns ready plays EVA once per charge, as own / ally / enemy (the TAYFUN with the nukes).
#include "GameClient/Eva.h"
#include <map>
void ZHEvaSuperweaponReady(UnsignedInt frame)
{
	static std::map<ObjectID, Bool> s_played;
	if (frame % 15 != 0 || !TheEva || !ThePlayerList)
		return;
	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if (!localPlayer)
		return;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->isEffectivelyDead() || obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			continue;
		for (BehaviorModule **module = obj->getBehaviorModules(); *module; ++module)
		{
			SpecialPowerModuleInterface *power = (*module)->getSpecialPower();
			if (!power || !power->getSpecialPowerTemplate())
				continue;
			const SpecialPowerType type = power->getSpecialPowerTemplate()->getSpecialPowerType();
			const Int kind = (type == SPECIAL_PARTICLE_UPLINK_CANNON || type == SUPW_SPECIAL_PARTICLE_UPLINK_CANNON || type == LAZR_SPECIAL_PARTICLE_UPLINK_CANNON) ? 0
				: (type == SPECIAL_NEUTRON_MISSILE || type == NUKE_SPECIAL_NEUTRON_MISSILE || type == SUPW_SPECIAL_NEUTRON_MISSILE || type == SUPR_SPECIAL_CRUISE_MISSILE) ? 1
				: type == SPECIAL_SCUD_STORM ? 2 : -1;
			if (kind < 0)
				continue;
			const Bool ready = power->isReady();
			Bool &played = s_played[obj->getID()];
			if (ready && !played)
			{
				const Int who = localPlayer == obj->getControllingPlayer() ? 0 : localPlayer->getRelationship(obj->getTeam()) != ENEMIES ? 1 : 2;
				static const EvaMessage messages[3][3] = {
					{ EVA_SuperweaponReady_Own_ParticleCannon, EVA_SuperweaponReady_Ally_ParticleCannon, EVA_SuperweaponReady_Enemy_ParticleCannon },
					{ EVA_SuperweaponReady_Own_Nuke, EVA_SuperweaponReady_Ally_Nuke, EVA_SuperweaponReady_Enemy_Nuke },
					{ EVA_SuperweaponReady_Own_ScudStorm, EVA_SuperweaponReady_Ally_ScudStorm, EVA_SuperweaponReady_Enemy_ScudStorm } };
				TheEva->setShouldPlay(messages[kind][who]);
			}
			played = ready;
		}
	}
}
// AI personalities (4 Nov 2026, plan 4.4): the original AI knows only its difficulty; the skirmish setup gives each AI
// slot a personality too (-lanslots "hard:aggressive", GameEngine.cpp TheZHAIPersonality) and this steers its army on top
// of the scripts. Aggressive: from 3 minutes on, every 45 s its idle combat units go hunting. Defensive: every 10 s its
// ground army is called home (guarding the command centre) unless an enemy is at its base, where they fight. Balanced: the
// original. A single-player skirmish only (no lockstep partner to keep in step with), never in a replay.
extern Int TheZHAIPersonality[8];
void ZHAIPersonalities(UnsignedInt frame)
{
	if (TheNetwork || !ThePlayerList || !TheGameLogic || (TheRecorder && TheRecorder->isPlaybackMode()) || frame % 30 != 0)
		return;
	const Int count = ThePlayerList->getPlayerCount();
	std::vector<Int> mode(count, 0);
	Bool any = FALSE;
	for (Int i = 0; i < count; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (!player || player->getPlayerType() != PLAYER_COMPUTER)
			continue;
		const AsciiString name = TheNameKeyGenerator->keyToName(player->getPlayerNameKey());
		Int slot = -1;
		if (name.startsWith("player") && sscanf(name.str() + 6, "%d", &slot) == 1 && slot >= 0 && slot < 8)
			mode[i] = TheZHAIPersonality[slot];
		any = any || mode[i] != 0;
	}
	if (!any)
		return;
	const Bool huntTime = frame >= 5400 && frame % 1350 == 0, homeTime = frame % 300 == 0;
	if (!huntTime && !homeTime)
		return;
	std::vector<Coord3D> home(count);
	std::vector<char> hasHome(count, 0), threatened(count, 0);
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		Player *owner = obj->getControllingPlayer();
		const Int index = owner ? owner->getPlayerIndex() : -1;
		if (index < 0 || index >= count || mode[index] != 2 || obj->isEffectivelyDead() || !obj->isKindOf(KINDOF_STRUCTURE))
			continue;
		if (!hasHome[index] || obj->isKindOf(KINDOF_COMMANDCENTER))
		{
			home[index] = *obj->getPosition();
			hasHome[index] = obj->isKindOf(KINDOF_COMMANDCENTER) ? 2 : 1;
		}
	}
	if (homeTime)
	{
		for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			Player *owner = obj->getControllingPlayer();
			if (!owner || obj->isEffectivelyDead() || obj->isKindOf(KINDOF_STRUCTURE))
				continue;
			for (Int i = 0; i < count; ++i)
			{
				if (mode[i] != 2 || !hasHome[i] || threatened[i] || owner->getRelationship(ThePlayerList->getNthPlayer(i)->getDefaultTeam()) != ENEMIES)
					continue;
				const Coord3D *p = obj->getPosition();
				const Real dx = p->x - home[i].x, dy = p->y - home[i].y;
				if (dx * dx + dy * dy < 900.0f * 900.0f)
					threatened[i] = 1;
			}
		}
	}
	Int hunting = 0, recalled = 0;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		Player *owner = obj->getControllingPlayer();
		const Int index = owner ? owner->getPlayerIndex() : -1;
		if (index < 0 || index >= count || !mode[index] || obj->isEffectivelyDead() || obj->getContainedBy() || !obj->getAIUpdateInterface())
			continue;
		if (!(obj->isKindOf(KINDOF_INFANTRY) || obj->isKindOf(KINDOF_VEHICLE) || obj->isKindOf(KINDOF_AIRCRAFT))
			|| obj->isKindOf(KINDOF_DOZER) || obj->isKindOf(KINDOF_HARVESTER) || obj->isKindOf(KINDOF_STRUCTURE))
			continue;
		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if (mode[index] == 1 && huntTime && ai->isIdle())
		{
			ai->aiHunt(CMD_FROM_AI);
			++hunting;
		}
		else if (mode[index] == 2 && homeTime && hasHome[index] && !threatened[index] && !obj->isKindOf(KINDOF_AIRCRAFT))
		{
			const Coord3D *p = obj->getPosition();
			const Real dx = p->x - home[index].x, dy = p->y - home[index].y;
			if (dx * dx + dy * dy > 1100.0f * 1100.0f)
			{
				// a ring round the command centre, each unit its own place
				const Real angle = Real(obj->getID() % 360) * 0.0174533f;
				Coord3D at = home[index];
				at.x += 260.0f * Cos(angle);
				at.y += 260.0f * Sin(angle);
				ai->aiGuardPosition(&at, GUARDMODE_NORMAL, CMD_FROM_AI);
				++recalled;
			}
		}
	}
	if (hunting || recalled)
	{
		printf("StateStream: AI personalities - %d hunting, %d called home (frame %u)\n", hunting, recalled, frame);
		fflush(stdout);
	}
}