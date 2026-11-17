/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// GameEngine.cpp /////////////////////////////////////////////////////////////////////////////////
// Implementation of the Game Engine singleton
// Author: Michael S. Booth, April 2001

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include <exception>

#include "Common/ActionManager.h"
#include "Common/AudioAffect.h"
#include "Common/BuildAssistant.h"
#include "Common/CRCDebug.h"
#include "Common/FramePacer.h"
#include "Common/Radar.h"
#include "Common/PlayerTemplate.h"
#include "Common/Team.h"
#include "Common/PlayerList.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/INI.h"
#include "Common/INIException.h"
#include "Common/MessageStream.h"
#include "Common/ThingFactory.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/LocalFileSystem.h"
#include "Common/GlobalData.h"
#include "Common/PerfTimer.h"
#include "Common/RandomValue.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ModuleFactory.h"
#include "Common/Debug.h"
#include "Common/GameState.h"
#include "Common/GameStateMap.h"
#include "Common/Science.h"
#include "Common/FunctionLexicon.h"
#include "Common/CommandLine.h"
#include "Common/DamageFX.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Recorder.h"
#include "Common/SpecialPower.h"
#include "Common/TerrainTypes.h"
#include "Common/Upgrade.h"
#include "Common/OptionPreferences.h"
#include "Common/Xfer.h"
#include "Common/XferCRC.h"
#include "Common/GameLOD.h"
#include "Common/Registry.h"
#include "Common/GameCommon.h"	// FOR THE ALLOW_DEBUG_CHEATS_IN_RELEASE #define

#include "GameLogic/Armor.h"
#include "GameLogic/AI.h"
#include "GameLogic/CaveSystem.h"
#include "GameLogic/CrateSystem.h"
#include "GameLogic/Damage.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/ObjectCreationList.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/RankInfo.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/SidesList.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/ChallengeGenerals.h"

#include "GameClient/ClientInstance.h"
#include "GameClient/FXList.h"
#include "GameClient/GameClient.h"
#include "GameClient/Keyboard.h"
#include "GameClient/Shell.h"
#include "GameClient/GameText.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/Water.h"
#include "GameClient/TerrainRoads.h"
#include "GameClient/MetaEvent.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Drawable.h"
#include "GameClient/GUICallbacks.h"

#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/ConnectionManager.h"
#include "GameNetwork/WOLBrowser/WebBrowser.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/GameSpy/GameResultsThread.h"

#include "Common/version.h"


//-------------------------------------------------------------------------------------------------

#ifdef DEBUG_CRC
class DeepCRCSanityCheck : public SubsystemInterface
{
public:
	DeepCRCSanityCheck() {}
	virtual ~DeepCRCSanityCheck() {}

	virtual void init() {}
	virtual void reset();
	virtual void update() {}

protected:
};

DeepCRCSanityCheck *TheDeepCRCSanityCheck = nullptr;

void DeepCRCSanityCheck::reset()
{
	static Int timesThrough = 0;
	static UnsignedInt lastCRC = 0;

	AsciiString fname;
	fname.format("%sCRCAfter%dMaps.dat", TheGlobalData->getPath_UserData().str(), timesThrough);
	UnsignedInt thisCRC = TheGameLogic->getCRC( CRC_RECALC, fname );

	DEBUG_LOG(("DeepCRCSanityCheck: CRC is %X", thisCRC));
	DEBUG_ASSERTCRASH(timesThrough == 0 || thisCRC == lastCRC,
		("CRC after reset did not match beginning CRC!\nNetwork games won't work after this.\nOld: 0x%8.8X, New: 0x%8.8X",
		lastCRC, thisCRC));
	lastCRC = thisCRC;

	timesThrough++;
}
#endif // DEBUG_CRC

//-------------------------------------------------------------------------------------------------
/// The GameEngine singleton instance
GameEngine *TheGameEngine = nullptr;

//-------------------------------------------------------------------------------------------------
SubsystemInterfaceList* TheSubsystemList = nullptr;

//-------------------------------------------------------------------------------------------------
template<class SUBSYSTEM>
void initSubsystem(
	SUBSYSTEM*& sysref,
	AsciiString name,
	SUBSYSTEM* sys,
	Xfer *pXfer,
	const char* path1 = nullptr,
	const char* path2 = nullptr)
{
	sysref = sys;
	TheSubsystemList->initSubsystem(sys, path1, path2, pXfer, name);
}

//-------------------------------------------------------------------------------------------------
extern HINSTANCE ApplicationHInstance;  ///< our application instance
extern CComModule _Module;

//-------------------------------------------------------------------------------------------------
static void updateTGAtoDDS();

//-------------------------------------------------------------------------------------------------
static void updateWindowTitle()
{
	// TheSuperHackers @tweak Now prints product and version information in the Window title.

	DEBUG_ASSERTCRASH(TheVersion != nullptr, ("TheVersion is null"));
	DEBUG_ASSERTCRASH(TheGameText != nullptr, ("TheGameText is null"));

	UnicodeString title;

	if (rts::ClientInstance::getInstanceId() > 1u)
	{
		UnicodeString str;
		str.format(L"Instance:%.2u", rts::ClientInstance::getInstanceId());
		title.concat(str);
	}

	UnicodeString productString = TheVersion->getUnicodeProductString();

	if (!productString.isEmpty())
	{
		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(productString);
	}

#if RTS_GENERALS
	const WideChar* defaultGameTitle = L"Command and Conquer Generals";
#elif RTS_ZEROHOUR
	const WideChar* defaultGameTitle = L"Command and Conquer Generals Zero Hour";
#endif
	UnicodeString gameTitle = TheGameText->FETCH_OR_SUBSTITUTE("GUI:Command&ConquerGenerals", defaultGameTitle);

	if (!gameTitle.isEmpty())
	{
		UnicodeString gameTitleFinal;
		UnicodeString gameVersion = TheVersion->getUnicodeVersion();

		if (productString.isEmpty())
		{
			gameTitleFinal = gameTitle;
		}
		else
		{
			UnicodeString gameTitleFormat = TheGameText->FETCH_OR_SUBSTITUTE("Version:GameTitle", L"for %ls");
			gameTitleFinal.format(gameTitleFormat.str(), gameTitle.str());
		}

		if (!title.isEmpty())
			title.concat(L" ");
		title.concat(gameTitleFinal.str());
		title.concat(L" ");
		title.concat(gameVersion.str());
	}

	if (!title.isEmpty())
	{
		AsciiString titleA;
		titleA.translate(title);	//get ASCII version for Win 9x

		extern HWND ApplicationHWnd;  ///< our application window handle
		if (ApplicationHWnd) {
			//Set it twice because Win 9x does not support SetWindowTextW.
			::SetWindowText(ApplicationHWnd, titleA.str());
			::SetWindowTextW(ApplicationHWnd, title.str());
		}
	}
}

//-------------------------------------------------------------------------------------------------
GameEngine::GameEngine()
{
	// initialize to non garbage values
	m_logicTimeAccumulator = 0.0f;
	m_quitting = FALSE;
	m_isActive = FALSE;

	_Module.Init(nullptr, ApplicationHInstance, nullptr);
}

//-------------------------------------------------------------------------------------------------
GameEngine::~GameEngine()
{
	//extern std::vector<std::string>	preloadTextureNamesGlobalHack;
	//preloadTextureNamesGlobalHack.clear();

	delete TheMapCache;
	TheMapCache = nullptr;

//	delete TheShell;
//	TheShell = nullptr;

	TheGameResultsQueue->endThreads();

	// TheSuperHackers @fix helmutbuhler 03/06/2025
	// Reset all subsystems before deletion to prevent crashing due to cross dependencies.
	reset();

	TheSubsystemList->shutdownAll();
	delete TheSubsystemList;
	TheSubsystemList = nullptr;

	delete TheSkirmishGameInfo;
	TheSkirmishGameInfo = nullptr;

	delete TheChallengeGameInfo;
	TheChallengeGameInfo = nullptr;

	delete TheNetwork;
	TheNetwork = nullptr;

	delete TheCommandList;
	TheCommandList = nullptr;

	delete TheNameKeyGenerator;
	TheNameKeyGenerator = nullptr;

	delete TheFileSystem;
	TheFileSystem = nullptr;

	delete TheGameLODManager;
	TheGameLODManager = nullptr;

	Drawable::killStaticImages();

	_Module.Term();

#ifdef PERF_TIMERS
	PerfGather::termPerfDump();
#endif
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isTimeFrozen()
{
	// TheSuperHackers @fix The time can no longer be frozen in Network games. It would disconnect the player.
	if (TheNetwork != nullptr)
		return false;

	if (TheTacticalView != nullptr)
	{
		if (TheTacticalView->isTimeFrozen() && !TheTacticalView->isCameraMovementFinished())
			return true;
	}

	if (TheScriptEngine != nullptr)
	{
		if (TheScriptEngine->isTimeFrozenDebug() || TheScriptEngine->isTimeFrozenScript())
			return true;
	}

	return false;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isGameHalted()
{
	if (TheNetwork != nullptr)
	{
		if (TheNetwork->isStalling())
			return true;
	}
	else
	{
		if (TheGameLogic != nullptr && TheGameLogic->isGamePaused())
			return true;
	}

	return false;
}

/** -----------------------------------------------------------------------------------------------
 * Initialize the game engine by initializing the GameLogic and GameClient.
 */
static Bool ParseLanSlots(std::vector<GameSlot> &slots, std::vector<AsciiString> &addresses);
// ue5 bridge: -skirmish <map> [-skirmishsides FactionAmerica,FactionChina] [-skirmishai easy|medium|hard|brutal]
// sets up a skirmish the way the skirmish menu does (slot 0 local human, other slots AI) and starts it.
static void StartCommandLineSkirmish()
{
	AsciiString map = TheGlobalData->m_skirmishMap;
	if (!map.endsWithNoCase(".map"))
		map.format("maps\\%s\\%s.map", TheGlobalData->m_skirmishMap.str(), TheGlobalData->m_skirmishMap.str());
	const MapMetaData *md = TheMapCache ? TheMapCache->findMap(map) : nullptr;
	if (!md)
	{
		DEBUG_LOG(("-skirmish: map %s not in the map cache", map.str()));
		printf("skirmish: map %s not found (cache %d maps)\n", map.str(), TheMapCache ? (Int)TheMapCache->size() : -1);
		if (TheMapCache)
			for (MapCache::const_iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it)
				if (strstr(it->first.str(), "crusade"))
					printf("  cache: %s\n", it->first.str());
		fflush(stdout);
		return;
	}
	TheSkirmishGameInfo = NEW SkirmishGameInfo;
	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->reset();
	TheSkirmishGameInfo->setLocalIP(TheSkirmishGameInfo->getSlot(0)->getIP());
	TheSkirmishGameInfo->enterGame();

	// ZH2026 skirmish setup (2 Nov 2026, the user: several enemies and teams, start spots picked on the map, starting cash,
	// AI levels): the menu sends the slot list in the LAN format (-lanslots, this player an H entry) with -lancash and
	// -lansuperweapons, and the sim runs it as an ordinary skirmish - no network.
	if (TheGlobalData->m_lanSlots.isNotEmpty() && TheGlobalData->m_lanSlot < 0)
	{
		std::vector<GameSlot> slots;
		std::vector<AsciiString> addresses;
		if (!ParseLanSlots(slots, addresses))
			return;
		Int human = -1, playing = 0;
		for (Int i = 0; i < (Int)slots.size(); ++i)
		{
			if (slots[i].isHuman() && human < 0)
				human = i;
			playing += slots[i].isOccupied() && slots[i].getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER ? 1 : 0;
		}
		if (human < 0 || playing < 1 || playing > md->m_numPlayers || (Int)slots.size() > MAX_SLOTS)
		{
			printf("skirmish: bad setup (%d slots, %d players for a %d-player map, human slot %d)\n", (Int)slots.size(), playing, md->m_numPlayers, human);
			fflush(stdout);
			return;
		}
		for (Int i = 0; i < (Int)slots.size(); ++i)
			TheSkirmishGameInfo->setSlot(i, slots[i]);
		TheSkirmishGameInfo->setLocalIP(slots[human].getIP());
		TheSkirmishGameInfo->setSeed(GetTickCount());
		Money cash = TheMultiplayerSettings->getDefaultStartingMoney();
		if (TheGlobalData->m_lanCash > 0)
		{
			cash.init();
			cash.deposit(TheGlobalData->m_lanCash, FALSE, FALSE);
		}
		TheSkirmishGameInfo->setStartingCash(cash);
		TheSkirmishGameInfo->setSuperweaponRestriction(TheGlobalData->m_lanSuperweapons ? 1 : 0);
		TheSkirmishGameInfo->setMap(map);
		TheSkirmishGameInfo->setMapCRC(md->m_CRC);
		TheSkirmishGameInfo->setMapSize(md->m_filesize);
		TheWritableGlobalData->m_mapName = map;
		TheWritableGlobalData->m_pendingFile = map;
		TheSkirmishGameInfo->startGame(0);
		InitRandom(TheSkirmishGameInfo->getSeed());
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
		msg->appendIntegerArgument(GAME_SKIRMISH);
		msg->appendIntegerArgument(DIFFICULTY_NORMAL);
		msg->appendIntegerArgument(0);
		msg->appendIntegerArgument(LOGICFRAMES_PER_SECOND);
		printf("skirmish: %s, %d players (slot list, local slot %d, cash %u, ceasefire %d s, speed %g)\n", map.str(), playing, human,
			cash.countMoney(), TheGlobalData->m_ceasefireSeconds, TheGlobalData->m_gameSpeed);
		fflush(stdout);
		return;
	}

	std::vector<AsciiString> sides;
	{
		AsciiString list = TheGlobalData->m_skirmishSides, token;
		while (list.nextToken(&token, ","))
			sides.push_back(token);
	}
	if (sides.size() < 2)
		sides = { AsciiString("FactionAmerica"), AsciiString("FactionChina") };
	// Observing: slot 0 is the local observer and every side gets an AI slot after it.
	const Bool observe = TheGlobalData->m_skirmishObserve;
	const Int first = observe ? 1 : 0;
	const Int players = std::min<Int>((Int)sides.size(), md->m_numPlayers);
	static const SlotState levels[] = { SLOT_EASY_AI, SLOT_MED_AI, SLOT_BRUTAL_AI, SLOT_BRUTAL_AI }; // the menu's Hard is BRUTAL_AI
	if (observe)
	{
		GameSlot watcher;
		watcher.setState(SLOT_PLAYER, UnicodeString(L"Observer"));
		watcher.setName(UnicodeString(L"Observer"));
		watcher.setPlayerTemplate(PLAYERTEMPLATE_OBSERVER);
		watcher.setStartPos(-1);
		watcher.setColor(-1);
		TheSkirmishGameInfo->setSlot(0, watcher);
	}
	for (Int i = 0; i < players; ++i)
	{
		GameSlot slot;
		const Bool human = !observe && i == 0;
		const UnicodeString name = human ? UnicodeString(L"Commander") : UnicodeString(L"AI");
		if (human)
			slot.setState(SLOT_PLAYER, name);
		else
			slot.setState(levels[std::max(0, std::min(3, (Int)TheGlobalData->m_skirmishAI))]);
		slot.setName(name);
		slot.setColor(i);
		slot.setStartPos(i);
		slot.setTeamNumber(-1);
		const Int faction = ThePlayerTemplateStore->getTemplateNumByName(sides[i]);
		slot.setPlayerTemplate(faction >= 0 ? faction : PLAYERTEMPLATE_RANDOM);
		TheSkirmishGameInfo->setSlot(first + i, slot);
	}
	TheSkirmishGameInfo->setSeed(GetTickCount());
	TheSkirmishGameInfo->setStartingCash(TheMultiplayerSettings->getDefaultStartingMoney());
	TheSkirmishGameInfo->setSuperweaponRestriction(0);
	TheSkirmishGameInfo->setMap(map);
	TheSkirmishGameInfo->setMapCRC(md->m_CRC);
	TheSkirmishGameInfo->setMapSize(md->m_filesize);

	TheWritableGlobalData->m_mapName = map;
	TheWritableGlobalData->m_pendingFile = map;
	TheSkirmishGameInfo->startGame(0);
	InitRandom(TheSkirmishGameInfo->getSeed());
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(GAME_SKIRMISH);
	msg->appendIntegerArgument(DIFFICULTY_NORMAL);
	msg->appendIntegerArgument(0);
	msg->appendIntegerArgument(LOGICFRAMES_PER_SECOND);
	printf("skirmish: %s, %d players\n", map.str(), players);
	fflush(stdout);
}
// ZH2026 LAN: the same slot list in every player's sim, the original lockstep network between them.
GameInfo *TheCommandLineNetGame = nullptr;

/** -lanslots: one entry per slot, ';' between them (the client's lobby builds it):
 *    H,<ip>:<port>,<side>,<color>,<start>,<team>,<name>   a player (this PC's own entry may be any unique address)
 *    A,<easy|medium|hard>,<side>,<color>,<start>,<team>   an AI, run by every sim alike
 *    X                                                    closed
 *  side: a player template name, "random" or "observer"; color/start: -1 random; team: -1 none, 0..3.
 *  The older -lanplayers ip:port,... with -skirmishsides becomes players with fixed colors and starts. */
/** AI personality per slot (0 balanced, 1 aggressive, 2 defensive), from -lanslots; StateStream.cpp acts on it. */
Int TheZHAIPersonality[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static Bool ParseLanSlots(std::vector<GameSlot> &slots, std::vector<AsciiString> &addresses)
{
	AsciiString list = TheGlobalData->m_lanSlots, entry;
	if (list.isEmpty())
	{
		std::vector<AsciiString> sides;
		AsciiString token, sideList = TheGlobalData->m_skirmishSides;
		while (sideList.nextToken(&token, ","))
			sides.push_back(token);
		AsciiString peers = TheGlobalData->m_lanPlayers;
		Int i = 0;
		while (peers.nextToken(&token, ","))
		{
			AsciiString line;
			line.format("H,%s,%s,%d,%d,-1,Player %d;", token.str(), i < (Int)sides.size() ? sides[i].str() : "random", i, i, i + 1);
			list.concat(line);
			++i;
		}
	}
	static const SlotState levels[] = { SLOT_EASY_AI, SLOT_MED_AI, SLOT_BRUTAL_AI };
	while (list.nextToken(&entry, ";"))
	{
		std::vector<AsciiString> f;
		AsciiString rest = entry, field;
		// The name (last field of a player) may hold commas: split the first six fields only.
		for (Int n = 0; n < 6 && rest.nextToken(&field, ","); ++n)
			f.push_back(field);
		AsciiString name = rest;
		if (name.startsWith(","))
			name = AsciiString(name.str() + 1);
		GameSlot slot;
		AsciiString address;
		if (f.empty() || f[0] == "X")
		{
			slot.setState(SLOT_CLOSED);
			slots.push_back(slot);
			addresses.push_back(address);
			continue;
		}
		if (f.size() < 6)
		{
			printf("lan: bad slot \"%s\"\n", entry.str());
			fflush(stdout);
			return FALSE;
		}
		UnicodeString uname;
		if (f[0] == "H")
		{
			unsigned a = 0, b = 0, c = 0, d = 0, p = 8088;
			if (sscanf(f[1].str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p) < 4)
			{
				printf("lan: bad player address %s\n", f[1].str());
				fflush(stdout);
				return FALSE;
			}
			const UnsignedInt ip = (a << 24) | (b << 16) | (c << 8) | d;
			uname.translate(name.isEmpty() ? AsciiString("Player") : name);
			slot.setState(SLOT_PLAYER, uname, ip);
			slot.setIP(ip);
			slot.setPort((UnsignedShort)p);
			address = f[1];
		}
		else
		{
			// <level>[:<personality>] (4 Nov 2026, plan 4.4): aggressive / defensive beside the original's balanced
			AsciiString levelName = f[1], personality;
			{
				AsciiString rest2 = f[1];
				rest2.nextToken(&levelName, ":");
				rest2.nextToken(&personality, ":");
			}
			if (slots.size() < 8)
				TheZHAIPersonality[slots.size()] = personality == "aggressive" ? 1 : personality == "defensive" ? 2 : 0;
			const Int level = levelName == "easy" ? 0 : levelName == "hard" ? 2 : 1;
			slot.setState(levels[level]);
			uname = level == 0 ? UnicodeString(L"Easy AI") : level == 2 ? UnicodeString(L"Hard AI") : UnicodeString(L"Medium AI");
		}
		// setState resets these, so after it.
		slot.setName(uname);
		slot.setColor(atoi(f[3].str()));
		slot.setStartPos(atoi(f[4].str()));
		slot.setTeamNumber(atoi(f[5].str()));
		Int faction = PLAYERTEMPLATE_RANDOM;
		if (f[2] == "observer")
			faction = PLAYERTEMPLATE_OBSERVER;
		else if (f[2] != "random")
		{
			const Int found = ThePlayerTemplateStore->getTemplateNumByName(f[2]);
			faction = found >= 0 ? found : PLAYERTEMPLATE_RANDOM;
		}
		slot.setPlayerTemplate(faction);
		if (faction == PLAYERTEMPLATE_OBSERVER)
		{
			slot.setStartPos(-1);
			slot.setTeamNumber(-1);
		}
		slot.setMapAvailability(TRUE);
		slot.setAccept();
		slots.push_back(slot);
		addresses.push_back(address);
	}
	return TRUE;
}

// ZH2026 campaign (3 Nov 2026, the user: port the original campaign, new missions come on top later): -campaign <name>
// [-campaignmission <mission>] [-campaigndifficulty 0|1|2] starts a campaign.ini mission the way the original single
// player and Generals Challenge menus do: the campaign manager points at the mission, a challenge campaign plays as its
// PlayerFaction (TheChallengeGameInfo slot 0, as ChallengeMenu.cpp setGeneralCampaign), the script engine and campaign
// get the difficulty, and the logic MSG_NEW_GAME GAME_SINGLE_PLAYER with the solo missions' frame cap.
static void StartCommandLineCampaign()
{
	if (!TheCampaignManager)
		return;
	if (TheGlobalData->m_campaignMission.isNotEmpty())
		TheCampaignManager->setCampaignAndMission(TheGlobalData->m_campaignName, TheGlobalData->m_campaignMission);
	else
		TheCampaignManager->setCampaign(TheGlobalData->m_campaignName);
	Campaign *campaign = TheCampaignManager->getCurrentCampaign();
	if (!campaign || !TheCampaignManager->getCurrentMission())
	{
		printf("campaign: %s / %s not found in campaign.ini\n", TheGlobalData->m_campaignName.str(), TheGlobalData->m_campaignMission.str());
		fflush(stdout);
		return;
	}
	const Int level = TheGlobalData->m_campaignDifficulty < 0 ? 0 : TheGlobalData->m_campaignDifficulty > 2 ? 2 : TheGlobalData->m_campaignDifficulty;
	const GameDifficulty difficulty = (GameDifficulty)(DIFFICULTY_EASY + level);
	TheCampaignManager->setGameDifficulty(difficulty);
	if (TheScriptEngine)
		TheScriptEngine->setGlobalDifficulty(difficulty);
	const AsciiString map = TheCampaignManager->getCurrentMap();
	if (campaign->isChallengeCampaign())
	{
		// The general played (CHALLENGE_0 names it; the others take it from their ChallengeMode.ini persona, as the menu does).
		AsciiString faction = campaign->m_playerFactionName;
		for (Int g = 0; faction.isEmpty() && TheChallengeGenerals && g < NUM_GENERALS; ++g)
			if (TheChallengeGenerals->getChallengeGenerals()[g].getCampaign().compareNoCase(campaign->m_name) == 0)
				faction = TheChallengeGenerals->getChallengeGenerals()[g].getPlayerTemplateName();
		const Int templateNum = ThePlayerTemplateStore->getTemplateNumByName(faction);
		if (TheChallengeGenerals)
			TheChallengeGenerals->setCurrentPlayerTemplateNum(templateNum);
		delete TheChallengeGameInfo;
		TheChallengeGameInfo = NEW SkirmishGameInfo;
		TheChallengeGameInfo->init();
		TheChallengeGameInfo->clearSlotList();
		TheChallengeGameInfo->reset();
		TheChallengeGameInfo->setLocalIP(TheChallengeGameInfo->getSlot(0)->getIP()); // as the skirmish setup: slot 0 is this player
		TheChallengeGameInfo->enterGame();
		const PlayerTemplate *pt = templateNum >= 0 ? ThePlayerTemplateStore->getNthPlayerTemplate(templateNum) : nullptr;
		GameSlot slot;
		slot.setState(SLOT_PLAYER, pt ? pt->getDisplayName() : UnicodeString(L"Commander"));
		slot.setPlayerTemplate(templateNum);
		TheChallengeGameInfo->setSlot(0, slot);
		TheChallengeGameInfo->setMap(map);
		TheChallengeGameInfo->setSeed(GetTickCount());
		TheChallengeGameInfo->setStartingCash(TheMultiplayerSettings->getDefaultStartingMoney());
	}
	TheWritableGlobalData->m_mapName = map;
	TheWritableGlobalData->m_pendingFile = map;
	TheWritableGlobalData->m_shellMapOn = FALSE;
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
	msg->appendIntegerArgument(difficulty);
	msg->appendIntegerArgument(TheCampaignManager->getRankPoints());
	msg->appendIntegerArgument(LOGICFRAMES_PER_SECOND);
	InitRandom(0);
	printf("campaign: %s %s (%s), difficulty %d%s\n", campaign->m_name.str(), TheCampaignManager->getCurrentMission()->m_name.str(), map.str(), level,
		campaign->isChallengeCampaign() ? ", challenge" : "");
	fflush(stdout);
}

static void StartCommandLineLanGame()
{
	AsciiString map = TheGlobalData->m_skirmishMap;
	if (!map.endsWithNoCase(".map"))
		map.format("maps\\%s\\%s.map", TheGlobalData->m_skirmishMap.str(), TheGlobalData->m_skirmishMap.str());
	const MapMetaData *md = TheMapCache ? TheMapCache->findMap(map) : nullptr;
	if (!md)
	{
		printf("lan: map %s not found\n", map.str());
		fflush(stdout);
		return;
	}
	std::vector<GameSlot> slots;
	std::vector<AsciiString> addresses;
	if (!ParseLanSlots(slots, addresses))
		return;
	const Int local = TheGlobalData->m_lanSlot;
	Int humans = 0, playing = 0;
	for (const GameSlot &s : slots)
	{
		humans += s.isHuman() ? 1 : 0;
		playing += s.isOccupied() && s.getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER ? 1 : 0;
	}
	if ((Int)slots.size() > MAX_SLOTS || humans < 1 || playing > md->m_numPlayers || local < 0 || local >= (Int)slots.size() || !slots[local].isHuman())
	{
		printf("lan: bad setup (%d slots, %d players for a %d-player map, local slot %d)\n", (Int)slots.size(), playing, md->m_numPlayers, local);
		fflush(stdout);
		return;
	}
	SkirmishGameInfo *game = NEW SkirmishGameInfo;
	game->init();
	game->clearSlotList();
	game->enterGame(); // resets the slots, so before they are filled
	for (Int i = 0; i < (Int)slots.size(); ++i)
		game->setSlot(i, slots[i]);
	const UnsignedInt localIP = slots[local].getIP();
	const UnsignedShort localPort = slots[local].getPort();
	game->setLocalIP(localIP);
	game->setSeed(TheGlobalData->m_lanSeed ? TheGlobalData->m_lanSeed : 2026);
	Money cash = TheMultiplayerSettings->getDefaultStartingMoney();
	if (TheGlobalData->m_lanCash > 0)
	{
		cash.init();
		cash.deposit(TheGlobalData->m_lanCash, FALSE, FALSE);
	}
	game->setStartingCash(cash);
	game->setSuperweaponRestriction(TheGlobalData->m_lanSuperweapons ? 1 : 0);
	game->setMap(map);
	game->setMapCRC(md->m_CRC);
	game->setMapSize(md->m_filesize);
	TheCommandLineNetGame = game;
	printf("lan: local %s -> local slot %d, %d players, %d slots\n", addresses[local].str(), game->getLocalSlotNum(), humans, (Int)slots.size());
	fflush(stdout);

	delete TheNetwork;
	TheNetwork = NetworkInterface::createNetwork();
	TheNetwork->init();
	ConnectionManager::s_bindAnyAddress = TRUE;
	TheNetwork->setLocalAddress(localIP, localPort);
	TheNetwork->initTransport();
	TheNetwork->parseUserList(game);

	TheWritableGlobalData->m_mapName = map;
	TheWritableGlobalData->m_pendingFile = map;
	game->startGame(0);
	InitRandom(game->getSeed());
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(GAME_LAN);
	TheWritableGlobalData->m_useFpsLimit = false;
	printf("lan: %s, slot %d, seed %u, cash %u, superweapons %s\n", map.str(), local, game->getSeed(), cash.countMoney(),
		TheGlobalData->m_lanSuperweapons ? "limited" : "free");
	fflush(stdout);
}

/** -dumpmapcache <file>: the multiplayer maps (players, start spots, extent), colors and starting cash choices as
 *  JSON for the client's lobby (Tools/pipeline/map_info.py), then the sim exits. */
static void DumpMapCache(const AsciiString &file)
{
	FILE *out = fopen(file.str(), "w");
	if (!out)
		return;
	fprintf(out, "{\"maps\":[");
	Bool first = TRUE;
	for (MapCache::const_iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it)
	{
		const MapMetaData &md = it->second;
		if (!md.m_isMultiplayer || md.m_numPlayers < 1)
			continue;
		AsciiString display;
		display.translate(md.m_displayName);
		fprintf(out, "%s\n{\"file\":\"", first ? "" : ",");
		for (const char *c = it->first.str(); *c; ++c)
			fprintf(out, *c == '\\' ? "/" : *c == '"' ? "'" : "%c", *c);
		fprintf(out, "\",\"name\":\"");
		for (const char *c = display.str(); *c; ++c)
			fprintf(out, *c == '"' || *c == '\\' ? "'" : "%c", *c);
		fprintf(out, "\",\"players\":%d,\"official\":%d,\"extent\":[%.1f,%.1f,%.1f,%.1f],\"starts\":[", md.m_numPlayers, md.m_isOfficial ? 1 : 0,
			md.m_extent.lo.x, md.m_extent.lo.y, md.m_extent.hi.x, md.m_extent.hi.y);
		for (Int p = 1; p <= md.m_numPlayers; ++p)
		{
			AsciiString key;
			key.format("Player_%d_Start", p);
			WaypointMap::const_iterator w = md.m_waypoints.find(key);
			if (w == md.m_waypoints.end())
				fprintf(out, "%snull", p > 1 ? "," : "");
			else
				fprintf(out, "%s[%.1f,%.1f]", p > 1 ? "," : "", w->second.x, w->second.y);
		}
		fprintf(out, "]}");
		first = FALSE;
	}
	fprintf(out, "],\n\"colors\":[");
	for (Int i = 0; i < TheMultiplayerSettings->getNumColors(); ++i)
	{
		MultiplayerColorDefinition *def = TheMultiplayerSettings->getColor(i);
		const RGBColor rgb = def->getRGBValue();
		fprintf(out, "%s{\"name\":\"%s\",\"rgb\":[%d,%d,%d]}", i ? "," : "", def->getTooltipName().str(), (Int)(rgb.red * 255), (Int)(rgb.green * 255), (Int)(rgb.blue * 255));
	}
	fprintf(out, "],\n\"cash\":[");
	const MultiplayerStartingMoneyList &money = TheMultiplayerSettings->getStartingMoneyList();
	for (size_t i = 0; i < money.size(); ++i)
		fprintf(out, "%s%u", i ? "," : "", money[i].countMoney());
	fprintf(out, "],\"defaultCash\":%u}\n", TheMultiplayerSettings->getDefaultStartingMoney().countMoney());
	fclose(out);
	printf("mapcache: written to %s\n", file.str());
	fflush(stdout);
}
void GameEngine::init()
{
	try {
		//create an INI object to use for loading stuff
		INI ini;

#ifdef DEBUG_LOGGING
		if (TheVersion)
		{
			DEBUG_LOG(("================================================================================"));
			DEBUG_LOG(("Generals version %s", TheVersion->getAsciiVersion().str()));
			DEBUG_LOG(("Build date: %s", TheVersion->getAsciiBuildTime().str()));
			DEBUG_LOG(("Build location: %s", TheVersion->getAsciiBuildLocation().str()));
			DEBUG_LOG(("Build user: %s", TheVersion->getAsciiBuildUser().str()));
			DEBUG_LOG(("Build git revision: %s", TheVersion->getAsciiGitCommitCount().str()));
			DEBUG_LOG(("Build git version: %s", TheVersion->getAsciiGitTagOrHash().str()));
			DEBUG_LOG(("Build git commit time: %s", TheVersion->getAsciiGitCommitTime().str()));
			DEBUG_LOG(("Build git commit author: %s", Version::getGitCommitAuthorName()));
			DEBUG_LOG(("================================================================================"));
		}
#endif

	#if defined(PERF_TIMERS) || defined(DUMP_PERF_STATS)
		DEBUG_LOG(("Calculating CPU frequency for performance timers."));
		InitPrecisionTimer();
	#endif
	#ifdef PERF_TIMERS
		PerfGather::initPerfDump("AAAPerfStats", PerfGather::PERF_NETTIME);
	#endif




	#ifdef DUMP_PERF_STATS////////////////////////////////////////////////////////////
	__int64 startTime64;//////////////////////////////////////////////////////////////
	__int64 endTime64,freq64;///////////////////////////////////////////////////////////
	GetPrecisionTimerTicksPerSec(&freq64);///////////////////////////////////////////////
	GetPrecisionTimer(&startTime64);////////////////////////////////////////////////////
  char Buf[256];//////////////////////////////////////////////////////////////////////
	#endif//////////////////////////////////////////////////////////////////////////////


		TheSubsystemList = MSGNEW("GameEngineSubsystem") SubsystemInterfaceList;

		TheSubsystemList->addSubsystem(this);

		// initialize the random number system
		InitRandom();

		// Create the low-level file system interface
		TheFileSystem = createFileSystem();

		// not part of the subsystem list, because it should normally never be reset!
		TheNameKeyGenerator = MSGNEW("GameEngineSubsystem") NameKeyGenerator;
		TheNameKeyGenerator->init();


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheNameKeyGenerator  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		// not part of the subsystem list, because it should normally never be reset!
		TheCommandList = MSGNEW("GameEngineSubsystem") CommandList;
		TheCommandList->init();

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheCommandList  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		XferCRC xferCRC;
		xferCRC.open("lightCRC");


		initSubsystem(TheLocalFileSystem, "TheLocalFileSystem", createLocalFileSystem(), nullptr);


    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheLocalFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheArchiveFileSystem, "TheArchiveFileSystem", createArchiveFileSystem(), nullptr); // this MUST come after TheLocalFileSystem creation

    	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheArchiveFileSystem  = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		DEBUG_ASSERTCRASH(TheWritableGlobalData,("TheWritableGlobalData expected to be created"));
		initSubsystem(TheWritableGlobalData, "TheWritableGlobalData", TheWritableGlobalData, &xferCRC, "Data\\INI\\Default\\GameData", "Data\\INI\\GameData");
		TheWritableGlobalData->parseCustomDefinition();


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After  TheWritableGlobalData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



	#if defined(RTS_DEBUG)
		// If we're in Debug, load the Debug settings as well.
		ini.loadFileDirectory( "Data\\INI\\GameDataDebug", INI_LOAD_OVERWRITE, nullptr );
	#endif

		// special-case: parse command-line parameters after loading global data
		CommandLine::parseCommandLineForEngineInit();

		TheArchiveFileSystem->loadMods();

		// doesn't require resets so just create a single instance here.
		TheGameLODManager = MSGNEW("GameEngineSubsystem") GameLODManager;
		TheGameLODManager->init();

		// after parsing the command line, we may want to perform dds stuff. Do that here.
		if (TheGlobalData->m_shouldUpdateTGAToDDS) {
			// update any out of date targas here.
			updateTGAtoDDS();
		}

		// read the water settings from INI (must do prior to initing GameClient, apparently)
		ini.loadFileDirectory( "Data\\INI\\Default\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Water", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Default\\Weather", INI_LOAD_OVERWRITE, &xferCRC );
		ini.loadFileDirectory( "Data\\INI\\Weather", INI_LOAD_OVERWRITE, &xferCRC );



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After water INI's = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#ifdef DEBUG_CRC
		initSubsystem(TheDeepCRCSanityCheck, "TheDeepCRCSanityCheck", MSGNEW("GameEngineSubystem") DeepCRCSanityCheck, nullptr);
#endif // DEBUG_CRC
		initSubsystem(TheGameText, "TheGameText", CreateGameTextInterface(), nullptr);
		updateWindowTitle();

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameText = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0xA1E7F8E6)
			TheNameKeyGenerator->verifyNameKeyID(1);
#endif

		initSubsystem(TheScienceStore,"TheScienceStore", MSGNEW("GameEngineSubsystem") ScienceStore(), &xferCRC, "Data\\INI\\Default\\Science", "Data\\INI\\Science");
		initSubsystem(TheMultiplayerSettings,"TheMultiplayerSettings", MSGNEW("GameEngineSubsystem") MultiplayerSettings(), &xferCRC, "Data\\INI\\Default\\Multiplayer", "Data\\INI\\Multiplayer");
		initSubsystem(TheTerrainTypes,"TheTerrainTypes", MSGNEW("GameEngineSubsystem") TerrainTypeCollection(), &xferCRC, "Data\\INI\\Default\\Terrain", "Data\\INI\\Terrain");
		initSubsystem(TheTerrainRoads,"TheTerrainRoads", MSGNEW("GameEngineSubsystem") TerrainRoadCollection(), &xferCRC, "Data\\INI\\Default\\Roads", "Data\\INI\\Roads");
		initSubsystem(TheGlobalLanguageData,"TheGlobalLanguageData",MSGNEW("GameEngineSubsystem") GlobalLanguage, nullptr); // must be before the game text
		TheGlobalLanguageData->parseCustomDefinition();
	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGlobalLanguageData = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////
		initSubsystem(TheAudio,"TheAudio", createAudioManager(TheGlobalData->m_headless), nullptr);

#if RTS_ZEROHOUR && RETAIL_COMPATIBLE_CRC
		TheNameKeyGenerator->syncNameKeyID();
#endif

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheAudio = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFunctionLexicon,"TheFunctionLexicon", createFunctionLexicon(), nullptr);
		initSubsystem(TheModuleFactory,"TheModuleFactory", createModuleFactory(), nullptr);
		initSubsystem(TheMessageStream,"TheMessageStream", createMessageStream(), nullptr);
		initSubsystem(TheSidesList,"TheSidesList", MSGNEW("GameEngineSubsystem") SidesList(), nullptr);
		initSubsystem(TheCaveSystem,"TheCaveSystem", MSGNEW("GameEngineSubsystem") CaveSystem(), nullptr);
		initSubsystem(TheRankInfoStore,"TheRankInfoStore", MSGNEW("GameEngineSubsystem") RankInfoStore(), &xferCRC, nullptr, "Data\\INI\\Rank");
		initSubsystem(ThePlayerTemplateStore,"ThePlayerTemplateStore", MSGNEW("GameEngineSubsystem") PlayerTemplateStore(), &xferCRC, "Data\\INI\\Default\\PlayerTemplate", "Data\\INI\\PlayerTemplate");
		initSubsystem(TheParticleSystemManager,"TheParticleSystemManager", createParticleSystemManager(TheGlobalData->m_headless), nullptr);

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheParticleSystemManager = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheFXListStore,"TheFXListStore", MSGNEW("GameEngineSubsystem") FXListStore(), &xferCRC, "Data\\INI\\Default\\FXList", "Data\\INI\\FXList");
		initSubsystem(TheWeaponStore,"TheWeaponStore", MSGNEW("GameEngineSubsystem") WeaponStore(), &xferCRC, nullptr, "Data\\INI\\Weapon");
		initSubsystem(TheObjectCreationListStore,"TheObjectCreationListStore", MSGNEW("GameEngineSubsystem") ObjectCreationListStore(), &xferCRC, "Data\\INI\\Default\\ObjectCreationList", "Data\\INI\\ObjectCreationList");
		initSubsystem(TheLocomotorStore,"TheLocomotorStore", MSGNEW("GameEngineSubsystem") LocomotorStore(), &xferCRC, nullptr, "Data\\INI\\Locomotor");
		initSubsystem(TheSpecialPowerStore,"TheSpecialPowerStore", MSGNEW("GameEngineSubsystem") SpecialPowerStore(), &xferCRC, "Data\\INI\\Default\\SpecialPower", "Data\\INI\\SpecialPower");
		initSubsystem(TheDamageFXStore,"TheDamageFXStore", MSGNEW("GameEngineSubsystem") DamageFXStore(), &xferCRC, nullptr, "Data\\INI\\DamageFX");
		initSubsystem(TheArmorStore,"TheArmorStore", MSGNEW("GameEngineSubsystem") ArmorStore(), &xferCRC, nullptr, "Data\\INI\\Armor");
		initSubsystem(TheBuildAssistant,"TheBuildAssistant", MSGNEW("GameEngineSubsystem") BuildAssistant, nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheBuildAssistant = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////



		initSubsystem(TheThingFactory,"TheThingFactory", createThingFactory(), &xferCRC, "Data\\INI\\Default\\Object", "Data\\INI\\Object");

	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheThingFactory = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


#if RETAIL_COMPATIBLE_CRC
		if (xferCRC.getCRC() == 0x6209AF6E)
			TheNameKeyGenerator->verifyNameKeyID(2265);
#endif

		initSubsystem(TheUpgradeCenter,"TheUpgradeCenter", MSGNEW("GameEngineSubsystem") UpgradeCenter, &xferCRC, "Data\\INI\\Default\\Upgrade", "Data\\INI\\Upgrade");
		initSubsystem(TheGameClient,"TheGameClient", createGameClient(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameClient = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		initSubsystem(TheAI,"TheAI", MSGNEW("GameEngineSubsystem") AI(), &xferCRC,  "Data\\INI\\Default\\AIData", "Data\\INI\\AIData");
		initSubsystem(TheGameLogic,"TheGameLogic", createGameLogic(), nullptr);
		initSubsystem(TheTeamFactory,"TheTeamFactory", MSGNEW("GameEngineSubsystem") TeamFactory(), nullptr);
		initSubsystem(TheCrateSystem,"TheCrateSystem", MSGNEW("GameEngineSubsystem") CrateSystem(), &xferCRC, "Data\\INI\\Default\\Crate", "Data\\INI\\Crate");
		initSubsystem(ThePlayerList,"ThePlayerList", MSGNEW("GameEngineSubsystem") PlayerList(), nullptr);
		initSubsystem(TheRecorder,"TheRecorder", createRecorder(), nullptr);
		initSubsystem(TheRadar,"TheRadar", createRadar(TheGlobalData->m_headless), nullptr);
		initSubsystem(TheVictoryConditions,"TheVictoryConditions", createVictoryConditions(), nullptr);



	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheVictoryConditions = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		AsciiString fname;
		fname.format("Data\\%s\\CommandMap", GetRegistryLanguage().str());
		initSubsystem(TheMetaMap,"TheMetaMap", MSGNEW("GameEngineSubsystem") MetaMap(), nullptr, fname.str(), "Data\\INI\\CommandMap");

#if defined(RTS_DEBUG)
		ini.loadFileDirectory("Data\\INI\\CommandMapDebug", INI_LOAD_MULTIFILE, nullptr);
#endif

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
		ini.loadFileDirectory("Data\\INI\\CommandMapDemo", INI_LOAD_MULTIFILE, nullptr, INI::LoadFlags_SearchSubDirs); // Added in Zero Hour
#endif

		TheMetaMap->generateMetaMap();
		TheMetaMap->verifyMetaMap();


		initSubsystem(TheActionManager,"TheActionManager", MSGNEW("GameEngineSubsystem") ActionManager(), nullptr);
		//initSubsystem((CComObject<WebBrowser> *)TheWebBrowser,"(CComObject<WebBrowser> *)TheWebBrowser", (CComObject<WebBrowser> *)createWebBrowser(), nullptr);
		initSubsystem(TheGameStateMap,"TheGameStateMap", MSGNEW("GameEngineSubsystem") GameStateMap, nullptr );
		initSubsystem(TheGameState,"TheGameState", MSGNEW("GameEngineSubsystem") GameState, nullptr );

		// Create the interface for sending game results
		initSubsystem(TheGameResultsQueue,"TheGameResultsQueue", GameResultsInterface::createNewGameResultsInterface(), nullptr);


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheGameResultsQueue = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		xferCRC.close();
		TheWritableGlobalData->m_iniCRC = xferCRC.getCRC();
		DEBUG_LOG(("INI CRC is 0x%8.8X", TheGlobalData->m_iniCRC));

		TheSubsystemList->postProcessLoadAll();

		TheFramePacer->setFramesPerSecondLimit(TheGlobalData->m_framesPerSecondLimit);

		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_musicOn, AudioAffect_Music);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_soundsOn, AudioAffect_Sound);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_sounds3DOn, AudioAffect_Sound3D);
		TheAudio->setOn(TheGlobalData->m_audioOn && TheGlobalData->m_speechOn, AudioAffect_Speech);
		// ue5 bridge (28 Oct 2026): a headless sim streaming to the Unreal client keeps its audio manager running (the
		// client's sound records come from its events) but must not be heard - its Miles device played the original music
		// and effects under the client's mix, and on after the client closed. Zero volume still keeps every event in flight.
		// The client picks its own music (AZHSimWorld::TickMusic), so the sim's music is off outright.
		if (TheGlobalData->m_headless && TheGlobalData->m_streamStatePort > 0)
		{
			TheAudio->setOn(false, AudioAffect_Music);
			TheAudio->setVolume(0.0f, (AudioAffect)(AudioAffect_Music | AudioAffect_Sound | AudioAffect_Sound3D | AudioAffect_Speech));
		}

		// We're not in a network game yet, so set the network singleton to nullptr.
		TheNetwork = nullptr;

		//Create a default ini file for options if it doesn't already exist.
		//OptionPreferences prefs( TRUE );

		// If we turn m_quitting to FALSE here, then we throw away any requests to quit that
		// took place during loading. :-\ - jkmcd
		// If this really needs to take place, please make sure that pressing cancel on the audio
		// load music dialog will still cause the game to quit.
		// m_quitting = FALSE;

		// initialize the MapCache
		TheMapCache = MSGNEW("GameEngineSubsystem") MapCache;
		TheMapCache->updateCache();


	#ifdef DUMP_PERF_STATS///////////////////////////////////////////////////////////////////////////
	GetPrecisionTimer(&endTime64);//////////////////////////////////////////////////////////////////
	sprintf(Buf,"----------------------------------------------------------------------------After TheMapCache->updateCache = %f seconds",((double)(endTime64-startTime64)/(double)(freq64)));
  startTime64 = endTime64;//Reset the clock ////////////////////////////////////////////////////////
	DEBUG_LOG(("%s", Buf));////////////////////////////////////////////////////////////////////////////
	#endif/////////////////////////////////////////////////////////////////////////////////////////////


		if (TheGlobalData->m_buildMapCache)
		{
			// just quit, since the map cache has already updated
			//populateMapListbox(nullptr, true, true);
			m_quitting = TRUE;
		}

		// load the initial shell screen
		//TheShell->push( "Menus/MainMenu.wnd" );

		// This allows us to run a map from the command line
		if (TheGlobalData->m_initialFile.isEmpty() == FALSE)
		{
			AsciiString fname = TheGlobalData->m_initialFile;
			fname.toLower();

			if (fname.endsWithNoCase(".map"))
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
				TheWritableGlobalData->m_playIntro = FALSE;
				TheWritableGlobalData->m_pendingFile = TheGlobalData->m_initialFile;

				// shutdown the top, but do not pop it off the stack
	//			TheShell->hideShell();

				// send a message to the logic for a new game
				GameMessage *msg = TheMessageStream->appendMessage( GameMessage::MSG_NEW_GAME );
				msg->appendIntegerArgument(GAME_SINGLE_PLAYER);
				msg->appendIntegerArgument(DIFFICULTY_NORMAL);
				msg->appendIntegerArgument(0);
				InitRandom(0);
			}
		}

		if (TheGlobalData->m_dumpMapCache.isNotEmpty())
		{
			DumpMapCache(TheGlobalData->m_dumpMapCache);
			exit(0);
		}
		if (TheGlobalData->m_campaignName.isNotEmpty())
			StartCommandLineCampaign();
		else if (TheGlobalData->m_skirmishMap.isNotEmpty())
		{
			TheWritableGlobalData->m_shellMapOn = FALSE;
			if (TheGlobalData->m_lanSlot >= 0)
				StartCommandLineLanGame();
			else
				StartCommandLineSkirmish();
		}

		//
		if (TheMapCache && TheGlobalData->m_shellMapOn)
		{
			AsciiString lowerName = TheGlobalData->m_shellMapName;
			lowerName.toLower();

			MapCache::const_iterator it = TheMapCache->find(lowerName);
			if (it == TheMapCache->end())
			{
				TheWritableGlobalData->m_shellMapOn = FALSE;
			}
		}

	}
	catch (ErrorCode ec)
	{
		if (ec == ERROR_INVALID_D3D)
		{
			RELEASE_CRASHLOCALIZED("ERROR:D3DFailurePrompt", "ERROR:D3DFailureMessage");
		}
	}
	catch (INIException e)
	{
		if (e.mFailureMessage)
			RELEASE_CRASH((e.mFailureMessage));
		else
			RELEASE_CRASH(("Uncaught Exception during initialization."));

	}
	catch (...)
	{
		RELEASE_CRASH(("Uncaught Exception during initialization."));
	}

	resetSubsystems();

	HideControlBar();
}

/** -----------------------------------------------------------------------------------------------
	* Reset all necessary parts of the game engine to be ready to accept new game data
	*/
void GameEngine::reset()
{

	WindowLayout *background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
	DEBUG_ASSERTCRASH(background,("We Couldn't Load Menus/BlankWindow.wnd"));
	background->hide(FALSE);
	background->bringForward();
	background->getFirstWindow()->winClearStatus(WIN_STATUS_IMAGE);
	Bool deleteNetwork = false;
	if (TheGameLogic->isInMultiplayerGame())
		deleteNetwork = true;

	resetSubsystems();

	if (deleteNetwork)
	{
		DEBUG_ASSERTCRASH(TheNetwork, ("Deleting null TheNetwork!"));
		delete TheNetwork;
		TheNetwork = nullptr;
	}
	if(background)
	{
		background->destroyWindows();
		deleteInstance(background);
		background = nullptr;
	}
}

/// -----------------------------------------------------------------------------------------------
void GameEngine::resetSubsystems()
{
	// TheSuperHackers @fix xezon 09/06/2025 Reset GameLogic first to purge all world objects early.
	// This avoids potentially catastrophic issues when objects and subsystems have cross dependencies.
	TheGameLogic->reset();

	TheSubsystemList->resetAll();
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateGameLogic(UnsignedInt logicTimeQueryFlags)
{
	// This updates the paused game status of the game logic.
	TheGameLogic->preUpdate();

	TheFramePacer->setTimeFrozen(isTimeFrozen());
	TheFramePacer->setGameHalted(isGameHalted());

	if (TheNetwork != nullptr)
	{
		return canUpdateNetworkGameLogic();
	}
	else
	{
		return canUpdateRegularGameLogic(logicTimeQueryFlags);
	}
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateNetworkGameLogic()
{
	DEBUG_ASSERTCRASH(TheNetwork != nullptr, ("TheNetwork is null"));

	if (TheNetwork->isFrameDataReady())
	{
		// Important: The Network is definitely no longer stalling.
		TheFramePacer->setGameHalted(false);

		return true;
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
Bool GameEngine::canUpdateRegularGameLogic(UnsignedInt logicTimeQueryFlags)
{
	const Int logicTimeScaleFps = TheFramePacer->getActualLogicTimeScaleFps(logicTimeQueryFlags);

	if (logicTimeScaleFps <= 0)
	{
		return false;
	}

	const Int maxRenderFps = TheFramePacer->getActualFramesPerSecondLimit();

#if defined(_ALLOW_DEBUG_CHEATS_IN_RELEASE)
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode;
#else	//always allow this cheat key if we're in a replay game.
	const Bool useFastMode = TheGlobalData->m_TiVOFastMode && TheGameLogic->isInReplayGame();
#endif

	if (useFastMode || logicTimeScaleFps >= maxRenderFps)
	{
		// Logic time scale is uncapped or larger equal Render FPS. Update straight away.
		return true;
	}
	else
	{
		// TheSuperHackers @tweak xezon 06/08/2025
		// The logic time step is now decoupled from the render update.
		const Real targetFrameTime = 1.0f / logicTimeScaleFps;
		m_logicTimeAccumulator += min(TheFramePacer->getUpdateTime(), targetFrameTime);

		if (m_logicTimeAccumulator >= targetFrameTime)
		{
			m_logicTimeAccumulator -= targetFrameTime;
			return true;
		}
	}

	return false;
}

/// -----------------------------------------------------------------------------------------------
DECLARE_PERF_TIMER(GameEngine_update)

/** -----------------------------------------------------------------------------------------------
 * Update the game engine by updating the GameClient and GameLogic singletons.
 */
void GameEngine::update()
{
	USE_PERF_TIMER(GameEngine_update)
	// ZH2026 (3 Nov 2026, the user: save and load): -loadsave <file> in the headless sim (the intro/shell path that loads it
	// in the full game never runs headless): the save is loaded on the first update, then streams like any battle.
	if (TheGlobalData->m_headless && TheGlobalData->m_loadSaveGame.isNotEmpty() && TheGameState)
		TheGameState->loadQueuedSaveGame();
	{
		{
			// VERIFY CRC needs to be in this code block.  Please to not pull TheGameLogic->update() inside this block.
			VERIFY_CRC

			TheRadar->UPDATE();

			/// @todo Move audio init, update, etc, into GameClient update

			TheAudio->UPDATE();
			TheGameClient->UPDATE();
			TheMessageStream->propagateMessages();

			if (TheNetwork != nullptr)
			{
				TheNetwork->UPDATE();
			}
		}

		// TheSuperHackers @info Ignores frozen time because the script engine needs updating in the logic update regardless.
		if (canUpdateGameLogic(FramePacer::IgnoreFrozenTime))
		{
			TheGameLogic->UPDATE();

			if (!TheFramePacer->isTimeFrozen())
			{
				TheGameClient->step();
			}
		}
	}
}

// Horrible reference, but we really, really need to know if we are windowed.
extern bool DX8Wrapper_IsWindowed;
extern HWND ApplicationHWnd;

/** -----------------------------------------------------------------------------------------------
 * The "main loop" of the game engine. It will not return until the game exits.
 */
void GameEngine::execute()
{
#if defined(RTS_DEBUG)
	DWORD startTime = timeGetTime() / 1000;
#endif

	// pretty basic for now
	while( !m_quitting )
	{

		//if (TheGlobalData->m_vTune)
		{
#ifdef PERF_TIMERS
			PerfGather::resetAll();
#endif
		}

		{

#if defined(RTS_DEBUG)
			{
				// enter only if in benchmark mode
				if (TheGlobalData->m_benchmarkTimer > 0)
				{
					DWORD currentTime = timeGetTime() / 1000;
					if (TheGlobalData->m_benchmarkTimer < currentTime - startTime)
					{
						if (TheGameLogic->isInGame())
						{
							if (TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
							{
								TheRecorder->stopRecording();
							}
							TheGameLogic->clearGameData();
						}
						TheGameEngine->setQuitting(TRUE);
					}
				}
			}
#endif

			{
				try
				{
					// compute a frame
					update();
				}
				catch (INIException e)
				{
					// Release CRASH doesn't return, so don't worry about executing additional code.
					if (e.mFailureMessage)
						RELEASE_CRASH((e.mFailureMessage));
					else
						RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
				catch (ErrorCode ec)
				{
					RELEASE_CRASH(("Native error in GameEngine::update: 0x%08x%s", static_cast<unsigned>(ec),
                        ec==ERROR_OUT_OF_MEMORY?" (ERROR_OUT_OF_MEMORY)":""));
				}
				catch (const std::exception& e)
				{
					RELEASE_CRASH(("C++ exception in GameEngine::update: %s", e.what()));
				}
				catch (...)
				{
					// try to save info off
					try
					{
						if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_RECORD && TheRecorder->isMultiplayer())
							TheRecorder->cleanUpReplayFile();
					}
					catch (...)
					{
					}
					RELEASE_CRASH(("Uncaught Exception in GameEngine::update"));
				}
			}

			TheFramePacer->update();
		}

#ifdef PERF_TIMERS
		if (!m_quitting && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() && !TheGameLogic->isGamePaused())
		{
			PerfGather::dumpAll(TheGameLogic->getFrame());
			PerfGather::displayGraph(TheGameLogic->getFrame());
			PerfGather::resetAll();
		}
#endif

	}
}

/** -----------------------------------------------------------------------------------------------
	* Factory for the message stream
	*/
MessageStream *GameEngine::createMessageStream()
{
	// if you change this update the tools that use the engine systems
	// like GUIEdit, it creates a message stream to run in "test" mode
	return MSGNEW("GameEngineSubsystem") MessageStream;
}

//-------------------------------------------------------------------------------------------------
FileSystem *GameEngine::createFileSystem()
{
	return MSGNEW("GameEngineSubsystem") FileSystem;
}

//-------------------------------------------------------------------------------------------------
Bool GameEngine::isMultiplayerSession()
{
	return TheRecorder->isMultiplayer();
}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
#define CONVERT_EXEC1	"..\\Build\\nvdxt -list buildDDS.txt -dxt5 -full -outdir Art\\Textures > buildDDS.out"

void updateTGAtoDDS()
{
	// Here's the scoop. We're going to traverse through all of the files in the Art\Textures folder
	// and determine if there are any .tga files that are newer than associated .dds files. If there
	// are, then we will re-run the compression tool on them.

	File *fp = TheLocalFileSystem->openFile("buildDDS.txt", File::WRITE | File::CREATE | File::TRUNCATE | File::TEXT);
	if (!fp) {
		return;
	}

	FilenameList files;
	TheLocalFileSystem->getFileListInDirectory("Art\\Textures\\", "", "*.tga", files, TRUE);
	FilenameList::iterator it;
	for (it = files.begin(); it != files.end(); ++it) {
		AsciiString filenameTGA = *it;
		AsciiString filenameDDS = *it;
		FileInfo infoTGA;
		TheLocalFileSystem->getFileInfo(filenameTGA, &infoTGA);

		// skip the water textures, since they need to be NOT compressed
		filenameTGA.toLower();
		if (strstr(filenameTGA.str(), "caust"))
		{
			continue;
		}
		// and the recolored stuff.
		if (strstr(filenameTGA.str(), "zhca"))
		{
			continue;
		}

		// replace tga with dds
		filenameDDS.truncateBy(3); // tga
		filenameDDS.concat("dds");

		Bool needsToBeUpdated = FALSE;
		FileInfo infoDDS;
		if (TheFileSystem->doesFileExist(filenameDDS.str())) {
			TheFileSystem->getFileInfo(filenameDDS, &infoDDS);
			if (infoTGA.timestampHigh > infoDDS.timestampHigh ||
					(infoTGA.timestampHigh == infoDDS.timestampHigh &&
					 infoTGA.timestampLow > infoDDS.timestampLow)) {
				needsToBeUpdated = TRUE;
			}
		} else {
			needsToBeUpdated = TRUE;
		}

		if (!needsToBeUpdated) {
			continue;
		}

		filenameTGA.concat("\n");
		fp->write(filenameTGA.str(), filenameTGA.getLength());
	}

	fp->close();

	system(CONVERT_EXEC1);
}
