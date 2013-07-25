//////////////////////////////////////////////////////////////////////
// The Forgotten Server - a server application for the MMORPG Tibia
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////

#include "otpch.h"

#include "definitions.h"
#include <boost/asio.hpp>
#include "server.h"

#include <string>
#include <cstring>
#include <errno.h>
#include <iostream>
#include <iomanip>

#include "networkmessage.h"
#include "protocolgame.h"

#include <stdlib.h>
#include <time.h>
#include "game.h"

#include "iologindata.h"
#include "iomarket.h"

#if !defined(__WINDOWS__)
#include <signal.h> // for sigemptyset()
#endif

#include "monsters.h"
#include "commands.h"
#include "outfit.h"
#include "vocation.h"
#include "scriptmanager.h"
#include "configmanager.h"

#include "tools.h"
#include "ban.h"
#include "rsa.h"

#include "protocolgame.h"
#include "protocolold.h"
#include "protocollogin.h"
#include "status.h"
#include "admin.h"
#include "globalevent.h"
#include "mounts.h"
#include "house.h"

#include "databasemanager.h"

Dispatcher g_dispatcher;
Scheduler g_scheduler;

IPList serverIPs;

extern AdminProtocolConfig* g_adminConfig;
Ban g_bans;
Game g_game;
Commands commands;
Npcs g_npcs;
ConfigManager g_config;
Monsters g_monsters;
Vocations g_vocations;
RSA g_RSA;

boost::mutex g_loaderLock;
boost::condition_variable g_loaderSignal;
boost::unique_lock<boost::mutex> g_loaderUniqueLock(g_loaderLock);

#include "networkmessage.h"

void startupErrorMessage(const std::string& errorStr)
{
	std::cout << "> ERROR: " << errorStr << std::endl;
	getchar();
	exit(-1);
}

void mainLoader(int argc, char* argv[], ServiceManager* servicer);

void badAllocationHandler()
{
	// Use functions that only use stack allocation
	puts("Allocation failed, server out of memory.\nDecrease the size of your map or compile in 64 bits mode.\n");
	getchar();
	exit(-1);
}

int main(int argc, char* argv[])
{
	// Setup bad allocation handler
	std::set_new_handler(badAllocationHandler);

#ifndef WIN32
	// ignore sigpipe...
	struct sigaction sigh;
	sigh.sa_handler = SIG_IGN;
	sigh.sa_flags = 0;
	sigemptyset(&sigh.sa_mask);
	sigaction(SIGPIPE, &sigh, NULL);
#endif

	ServiceManager servicer;

	g_dispatcher.start();
	g_scheduler.start();

	g_dispatcher.addTask(createTask(boost::bind(mainLoader, argc, argv, &servicer)));

	g_loaderSignal.wait(g_loaderUniqueLock);

	if (servicer.is_running()) {
		std::cout << ">> " << g_config.getString(ConfigManager::SERVER_NAME) << " Server Online!" << std::endl << std::endl;
		servicer.run();
		g_scheduler.join();
		g_dispatcher.join();
	} else {
		std::cout << ">> No services running. The server is NOT online." << std::endl;
	}

	return 0;
}

void mainLoader(int argc, char* argv[], ServiceManager* services)
{
	//dispatcher thread
	g_game.setGameState(GAME_STATE_STARTUP);

	srand((unsigned int)OTSYS_TIME());
#ifdef WIN32
	SetConsoleTitle(STATUS_SERVER_NAME);
#endif
	std::cout << STATUS_SERVER_NAME << " - Version " << STATUS_SERVER_VERSION << std::endl;
	std::cout << "Compilied on " << __DATE__ << " " << __TIME__ << " for arch ";

#if defined(__amd64__) || defined(_M_X64)
	std::cout << "x64" << std::endl;
#elif defined(__i386__) || defined(_M_IX86) || defined(_X86_)
	std::cout << "x86" << std::endl;
#else
	std::cout << "unk" << std::endl;
#endif

	std::cout << std::endl;

	std::cout << "A server developed by " << STATUS_SERVER_DEVELOPERS << std::endl;
	std::cout << "Visit our forum for updates, support, and resources: http://otland.net/." << std::endl;
	std::cout << std::endl;

	// read global config
	std::cout << ">> Loading config" << std::endl;

	if (!g_config.loadFile("config.lua")) {
		startupErrorMessage("Unable to load config.lua!");
		return;
	}

#ifdef WIN32
	std::string defaultPriority = asLowerCaseString(g_config.getString(ConfigManager::DEFAULT_PRIORITY));
	if (defaultPriority == "realtime") {
		SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
	} else if (defaultPriority == "high") {
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	} else if (defaultPriority == "higher") {
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	}

	std::ostringstream mutexName;
	mutexName << "forgottenserver_" << g_config.getNumber(ConfigManager::LOGIN_PORT);
	CreateMutex(NULL, FALSE, mutexName.str().c_str());
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		startupErrorMessage("Another instance of The Forgotten Server is already running with the same login port, please shut it down first or change ports for this one.");
	}
#endif

	//set RSA key
	const char* p("14299623962416399520070177382898895550795403345466153217470516082934737582776038882967213386204600674145392845853859217990626450972452084065728686565928113");
	const char* q("7630979195970404721891201847792002125535401292779123937207447574596692788513647179235335529307251350570728407373705564708871762033017096809910315212884101");
	const char* d("46730330223584118622160180015036832148732986808519344675210555262940258739805766860224610646919605860206328024326703361630109888417839241959507572247284807035235569619173792292786907845791904955103601652822519121908367187885509270025388641700821735345222087940578381210879116823013776808975766851829020659073");
	g_RSA.setKey(p, q, d);

	std::cout << ">> Establishing database connection..." << std::flush;
	Database* db = Database::getInstance();
	if (!db->connect()) {
		startupErrorMessage("Failed to connect to database.");
		return;
	}

	std::cout << " MySQL " << db->getClientVersion() << std::endl;

	// run database manager
	std::cout << ">> Running database manager" << std::endl;
	DatabaseManager* dbManager = DatabaseManager::getInstance();

	if (!dbManager->isDatabaseSetup()) {
		startupErrorMessage("The database you have specified in config.lua is empty, please import the schema to the database.");
		return;
	}

	for (uint32_t version = dbManager->updateDatabase(); version != 0; version = dbManager->updateDatabase()) {
		std::cout << "> Database has been updated to version " << version << "." << std::endl;
	}

	dbManager->checkTriggers();
	dbManager->checkEncryption();

	if (g_config.getBoolean(ConfigManager::OPTIMIZE_DATABASE) && !dbManager->optimizeTables()) {
		std::cout << "> No tables were optimized." << std::endl;
	}

	//load vocations
	std::cout << ">> Loading vocations" << std::endl;

	if (!g_vocations.loadFromXml()) {
		startupErrorMessage("Unable to load vocations!");
	}

	//load commands
	std::cout << ">> Loading commands" << std::endl;

	if (!commands.loadFromXml()) {
		startupErrorMessage("Unable to load commands!");
	}

	// load item data
	std::cout << ">> Loading items" << std::endl;

	if (Item::items.loadFromOtb("data/items/items.otb")) {
		startupErrorMessage("Unable to load items (OTB)!");
	}

	if (!Item::items.loadFromXml()) {
		startupErrorMessage("Unable to load items (XML)!");
	}

	std::cout << ">> Loading script systems" << std::endl;

	if (!ScriptingManager::getInstance()->loadScriptSystems()) {
		startupErrorMessage("Failed to load script systems");
	}

	std::cout << ">> Loading monsters" << std::endl;

	if (!g_monsters.loadFromXml()) {
		startupErrorMessage("Unable to load monsters!");
	}

	std::cout << ">> Loading outfits" << std::endl;
	Outfits* outfits = Outfits::getInstance();

	if (!outfits->loadFromXml()) {
		startupErrorMessage("Unable to load outfits!");
	}

	g_adminConfig = new AdminProtocolConfig();
	std::cout << ">> Loading admin protocol config" << std::endl;

	if (!g_adminConfig->loadXMLConfig()) {
		startupErrorMessage("Unable to load admin protocol config!");
	}

	std::cout << ">> Loading experience stages" << std::endl;

	if (!g_game.loadExperienceStages()) {
		startupErrorMessage("Unable to load experience stages!");
	}

	std::string passwordType = asLowerCaseString(g_config.getString(ConfigManager::PASSWORDTYPE));

	if (passwordType == "md5") {
		g_config.setNumber(ConfigManager::PASSWORD_TYPE, PASSWORD_TYPE_MD5);
		std::cout << ">> Using MD5 passwords" << std::endl;
	} else if (passwordType == "sha1") {
		g_config.setNumber(ConfigManager::PASSWORD_TYPE, PASSWORD_TYPE_SHA1);
		std::cout << ">> Using SHA1 passwords" << std::endl;
	} else {
		g_config.setNumber(ConfigManager::PASSWORD_TYPE, PASSWORD_TYPE_PLAIN);
		std::cout << ">> Using plaintext passwords" << std::endl;
	}

	std::cout << ">> Checking world type... ";
	std::string worldType = asLowerCaseString(g_config.getString(ConfigManager::WORLD_TYPE));

	if (worldType == "pvp") {
		g_game.setWorldType(WORLD_TYPE_PVP);
	} else if (worldType == "no-pvp") {
		g_game.setWorldType(WORLD_TYPE_NO_PVP);
	} else if (worldType == "pvp-enforced") {
		g_game.setWorldType(WORLD_TYPE_PVP_ENFORCED);
	} else {
		std::cout << std::endl;

		std::ostringstream ss;
		ss << "> ERROR: Unknown world type: " << g_config.getString(ConfigManager::WORLD_TYPE) << ", valid world types are: pvp, no-pvp and pvp-enforced.";
		startupErrorMessage(ss.str());
	}

	std::cout << asUpperCaseString(worldType) << std::endl;

	std::cout << ">> Loading map" << std::endl;

	if (!g_game.loadMap(g_config.getString(ConfigManager::MAP_NAME))) {
		startupErrorMessage("Failed to load map");
	}

	std::cout << ">> Initializing gamestate" << std::endl;
	g_game.setGameState(GAME_STATE_INIT);

	// Tibia protocols
	services->add<ProtocolGame>(g_config.getNumber(ConfigManager::GAME_PORT));
	services->add<ProtocolLogin>(g_config.getNumber(ConfigManager::LOGIN_PORT));

	// OT protocols
	services->add<ProtocolAdmin>(g_config.getNumber(ConfigManager::ADMIN_PORT));
	services->add<ProtocolStatus>(g_config.getNumber(ConfigManager::STATUS_PORT));

	// Legacy protocols
	services->add<ProtocolOldLogin>(g_config.getNumber(ConfigManager::LOGIN_PORT));
	services->add<ProtocolOldGame>(g_config.getNumber(ConfigManager::LOGIN_PORT));

	int32_t autoSaveEachMinutes = g_config.getNumber(ConfigManager::AUTO_SAVE_EACH_MINUTES);

	if (autoSaveEachMinutes > 0) {
		g_scheduler.addEvent(createSchedulerTask(autoSaveEachMinutes * 1000 * 60, boost::bind(&Game::autoSave, &g_game)));
	}

	if (g_config.getBoolean(ConfigManager::SERVERSAVE_ENABLED)) {
		int32_t serverSaveHour = g_config.getNumber(ConfigManager::SERVERSAVE_H);

		if (serverSaveHour >= 0 && serverSaveHour <= 24) {
			time_t timeNow = time(NULL);
			tm* timeinfo = localtime(&timeNow);

			if (serverSaveHour == 0) {
				serverSaveHour = 23;
			} else {
				serverSaveHour--;
			}

			timeinfo->tm_hour = serverSaveHour;
			timeinfo->tm_min = 55;
			timeinfo->tm_sec = 0;
			time_t difference = (time_t)difftime(mktime(timeinfo), timeNow);

			if (difference < 0) {
				difference += 86400;
			}

			g_scheduler.addEvent(createSchedulerTask(difference * 1000, boost::bind(&Game::prepareServerSave, &g_game)));
		}
	}

	Houses::getInstance().payHouses();
	IOLoginData::getInstance()->updateHouseOwners();
	g_npcs.reload();

	if (g_config.getBoolean(ConfigManager::MARKET_ENABLED)) {
		g_game.checkExpiredMarketOffers();
		IOMarket::getInstance()->updateStatistics();
	}

	std::cout << ">> Loaded all modules, server starting up..." << std::endl;

	std::pair<uint32_t, uint32_t> IpNetMask;
	IpNetMask.first = inet_addr("127.0.0.1");
	IpNetMask.second = 0xFFFFFFFF;
	serverIPs.push_back(IpNetMask);

	char szHostName[128];
	if (gethostname(szHostName, 128) == 0) {
		hostent* he = gethostbyname(szHostName);

		if (he) {
			unsigned char** addr = (unsigned char**)he->h_addr_list;

			while (addr[0] != NULL) {
				IpNetMask.first = *(uint32_t*)(*addr);
				IpNetMask.second = 0xFFFFFFFF;
				serverIPs.push_back(IpNetMask);
				addr++;
			}
		}
	}

	std::string ip;
	ip = g_config.getString(ConfigManager::IP);

	uint32_t resolvedIp = inet_addr(ip.c_str());

	if (resolvedIp == INADDR_NONE) {
		struct hostent* he = gethostbyname(ip.c_str());

		if (he != 0) {
			resolvedIp = *(uint32_t*)he->h_addr;
		} else {
			std::ostringstream ss;
			ss << "ERROR: Cannot resolve " << ip << "!" << std::endl;
			startupErrorMessage(ss.str());
		}
	}

	IpNetMask.first = resolvedIp;
	IpNetMask.second = 0;
	serverIPs.push_back(IpNetMask);

#if !defined(WIN32) && !defined(__ROOT_PERMISSION__)
	if (getuid() == 0 || geteuid() == 0) {
		std::cout << "> WARNING: " << STATUS_SERVER_NAME << " has been executed as root user, it is recommended to execute is as a normal user." << std::endl;
	}
#endif

	g_game.start(services);
	g_game.setGameState(GAME_STATE_NORMAL);
	g_loaderSignal.notify_all();
}
