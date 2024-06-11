/*
 * This file is part of the FirelandsCore Project. See AUTHORS file for Copyright
 * information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/// \addtogroup Firelandsd Firelands Daemon
/// @{
/// \file

#include "AppenderDB.h"
#include "AsyncAcceptor.h"
#include "Banner.h"
#include "BattlegroundMgr.h"
#include "BigNumber.h"
#include "CliRunnable.h"
#include "Common.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLoader.h"
#include "DeadlineTimer.h"
#include "FCSoap.h"
#include "GitRevision.h"
#include "InstanceSaveMgr.h"
#include "IoContext.h"
#include "MapManager.h"
#include "Metric.h"
#include "ModuleMgr.h"
#include "ModulesScriptLoader.h"
#include "MySQLThreading.h"
#include "ObjectAccessor.h"
#include "OpenSSLCrypto.h"
#include "OutdoorPvP/OutdoorPvPMgr.h"
#include "ProcessPriority.h"
#include "RASession.h"
#include "RealmList.h"
#include "Resolver.h"
#include "ScriptLoader.h"
#include "ScriptMgr.h"
#include "ScriptReloadMgr.h"
#include "World.h"
#include "WorldSocket.h"
#include "WorldSocketMgr.h"

#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#if defined(OPENSSL_VERSION_MAJOR) && (OPENSSL_VERSION_MAJOR >= 3)
#include <openssl/provider.h>
#endif
#include <boost/dll/runtime_symbol_info.hpp>


#include <boost/asio/signal_set.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>

#include <csignal>
#include <iostream>

#if FC_PLATFORM == FC_PLATFORM_WINDOWS // ugly as hell
#pragma comment(lib, "iphlpapi.lib")
#endif

using namespace boost::program_options;
namespace fs = boost::filesystem;

#ifndef _FIRELANDS_CORE_CONFIG
#define _FIRELANDS_CORE_CONFIG "worldserver.conf"
#endif

#if FC_PLATFORM == FC_PLATFORM_WINDOWS
#include "ServiceWin32.h"
char serviceName[] = "worldserver";
char serviceLongName[] = "Firelands world service";
char serviceDescription[] = "Firelands World of Warcraft emulator world service";
/*
 * -1 - not in service mode
 *  0 - stopped
 *  1 - running
 *  2 - paused
 */
int m_ServiceStatus = -1;

#include <boost/dll/shared_library.hpp>
#include <timeapi.h>
#endif

class FreezeDetector
{
  public:
    FreezeDetector(Firelands::Asio::IoContext &ioContext, uint32 maxCoreStuckTime) : _timer(ioContext), _worldLoopCounter(0), _lastChangeMsTime(getMSTime()), _maxCoreStuckTimeInMs(maxCoreStuckTime)
    {
    }

    static void Start(std::shared_ptr<FreezeDetector> const &freezeDetector)
    {
        freezeDetector->_timer.expires_from_now(boost::posix_time::seconds(5));
        freezeDetector->_timer.async_wait(std::bind(&FreezeDetector::Handler, std::weak_ptr<FreezeDetector>(freezeDetector), std::placeholders::_1));
    }

    static void Handler(std::weak_ptr<FreezeDetector> freezeDetectorRef, boost::system::error_code const &error);

  private:
    Firelands::Asio::DeadlineTimer _timer;
    uint32 _worldLoopCounter;
    uint32 _lastChangeMsTime;
    uint32 _maxCoreStuckTimeInMs;
};

void SignalHandler(boost::system::error_code const &error, int signalNumber);

AsyncAcceptor *StartRaSocketAcceptor(Firelands::Asio::IoContext &ioContext);
bool StartDB();
void StopDB();
void WorldUpdateLoop();
void ClearOnlineAccounts();
void ShutdownCLIThread(std::thread *cliThread);
bool LoadRealmInfo(Firelands::Asio::IoContext &ioContext);
variables_map GetConsoleArguments(int argc, char **argv, fs::path &configFile, std::string &cfg_service);

/// Launch the Firelands server
extern int main(int argc, char **argv)
{
    signal(SIGABRT, &Firelands::AbortHandler);

    auto configFile = fs::absolute(_FIRELANDS_CORE_CONFIG);
    std::string configService;

    auto vm = GetConsoleArguments(argc, argv, configFile, configService);
    // exit if help or version is enabled
    if (vm.count("help") || vm.count("version"))
        return 0;

#if FC_PLATFORM == FC_PLATFORM_WINDOWS
    if (configService.compare("install") == 0)
        return WinServiceInstall() == true ? 0 : 1;
    else if (configService.compare("uninstall") == 0)
        return WinServiceUninstall() == true ? 0 : 1;
    else if (configService.compare("run") == 0)
        WinServiceRun();

    Optional<UINT> newTimerResolution;
    boost::system::error_code dllError;
    std::shared_ptr<boost::dll::shared_library> winmm(new boost::dll::shared_library("winmm.dll", dllError, boost::dll::load_mode::search_system_folders),
        [&](boost::dll::shared_library *lib)
        {
            try
            {
                if (newTimerResolution)
                    lib->get<decltype(timeEndPeriod)>("timeEndPeriod")(*newTimerResolution);
            }
            catch (std::exception const &)
            {
                // ignore
            }

            delete lib;
        });

    if (winmm->is_loaded())
    {
        try
        {
            auto timeGetDevCapsPtr = winmm->get<decltype(timeGetDevCaps)>("timeGetDevCaps");
            // setup timer resolution
            TIMECAPS timeResolutionLimits;
            if (timeGetDevCapsPtr(&timeResolutionLimits, sizeof(TIMECAPS)) == TIMERR_NOERROR)
            {
                auto timeBeginPeriodPtr = winmm->get<decltype(timeBeginPeriod)>("timeBeginPeriod");
                newTimerResolution = std::min(std::max(timeResolutionLimits.wPeriodMin, 1u), timeResolutionLimits.wPeriodMax);
                timeBeginPeriodPtr(*newTimerResolution);
            }
        }
        catch (std::exception const &e)
        {
            printf("Failed to initialize timer resolution: %s\n", e.what());
        }
    }

#endif

    sConfigMgr->Configure(configFile.generic_string(), std::vector<std::string>(argv, argv + argc), CONFIG_FILE_LIST);

    if (!sConfigMgr->LoadConfigsCore())
    {
        printf("Error loading Config file");
        return 1;
    }

    std::shared_ptr<Firelands::Asio::IoContext> ioContext = std::make_shared<Firelands::Asio::IoContext>();

    sLog->RegisterAppender<AppenderDB>();
    // If logs are supposed to be handled async then we need to pass the IoContext
    // into the Log singleton
    sLog->Initialize(sConfigMgr->GetBoolDefault("Log.Async.Enable", false) ? ioContext.get() : nullptr);

    Firelands::Banner::Show(
        "worldserver-daemon", [](char const *text) { LOG_INFO("server.worldserver", "%s", text); },
        []()
        {
            LOG_INFO("server.worldserver", "Using configuration file %s.", sConfigMgr->GetFilename().c_str());
            //LOG_INFO("server.worldserver", "Using SSL version: %s (library: %s)", OPENSSL_VERSION_TEXT, SSLeay_version(SSLEAY_VERSION));
            LOG_INFO("server.worldserver", "Using Boost version: %i.%i.%i", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        });

    OpenSSLCrypto::threadsSetup(boost::dll::program_location().remove_filename());

    std::shared_ptr<void> opensslHandle(nullptr, [](void *) { OpenSSLCrypto::threadsCleanup(); });

    // Seed the OpenSSL's PRNG here.
    // That way it won't auto-seed when calling BigNumber::SetRand and slow down
    // the first world login
    BigNumber seed;
    seed.SetRand(16 * 8);

    /// worldserver PID file creation
    std::string pidFile = sConfigMgr->GetStringDefault("PidFile", "");
    if (!pidFile.empty())
    {
        if (uint32 pid = CreatePIDFile(pidFile))
            LOG_INFO("server.worldserver", "Daemon PID: %u\n", pid);
        else
        {
            LOG_ERROR("server.worldserver", "Cannot create PID file %s.\n", pidFile.c_str());
            return 1;
        }
    }

    // Set signal handlers (this must be done before starting IoContext threads,
    // because otherwise they would unblock and exit)
    boost::asio::signal_set signals(*ioContext, SIGINT, SIGTERM);
#if FC_PLATFORM == FC_PLATFORM_WINDOWS
    signals.add(SIGBREAK);
#endif
    signals.async_wait(SignalHandler);

    // Start the Boost based thread pool
    int numThreads = sConfigMgr->GetIntDefault("ThreadPool", 1);
    std::shared_ptr<std::vector<std::thread>> threadPool(new std::vector<std::thread>(),
        [ioContext](std::vector<std::thread> *del)
        {
            ioContext->stop();
            for (std::thread &thr : *del)
                thr.join();

            delete del;
        });

    if (numThreads < 1)
        numThreads = 1;

    for (int i = 0; i < numThreads; ++i)
        threadPool->push_back(std::thread([ioContext]() { ioContext->run(); }));

    // Set process priority according to configuration settings
    SetProcessPriority("server.worldserver", sConfigMgr->GetIntDefault(CONFIG_PROCESSOR_AFFINITY, 0), sConfigMgr->GetBoolDefault(CONFIG_HIGH_PRIORITY, false));

    sConfigMgr->LoadConfigsModules();

    // Start the databases
    if (!StartDB())
        return 1;

    std::shared_ptr<void> dbHandle(nullptr, [](void *) { StopDB(); });

    if (vm.count("update-databases-only"))
        return 0;

    // Set server offline (not connectable)
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = flag | %u WHERE id = '%d'", REALM_FLAG_OFFLINE, realm.Id.Realm);

    LoadRealmInfo(*ioContext);

    sMetric->Initialize(realm.Name, *ioContext, []() { FC_METRIC_VALUE("online_players", sWorld->GetPlayerCount()); });

    FC_METRIC_EVENT("events", "Worldserver started", "");

    std::shared_ptr<void> sMetricHandle(nullptr,
        [](void *)
        {
            FC_METRIC_EVENT("events", "Worldserver shutdown", "");
            sMetric->Unload();
        });

    sScriptMgr->SetScriptLoader(AddScripts);
    sScriptMgr->SetModulesLoader(AddModulesScripts);

    std::shared_ptr<void> sScriptMgrHandle(nullptr,
        [](void *)
        {
            sScriptMgr->Unload();
            sScriptReloadMgr->Unload();
        });

    Firelands::Module::SetEnableModulesList(FC_MODULES_LIST);

    // Initialize the World
    sWorld->SetInitialWorldSettings();

    std::shared_ptr<void> mapManagementHandle(nullptr,
        [](void *)
        {
            // unload battleground templates before different singletons destroyed
            sBattlegroundMgr->DeleteAllBattlegrounds();

            sInstanceSaveMgr->Unload();
            sOutdoorPvPMgr->Die(); // unload it before MapManager
            sMapMgr->UnloadAll();  // unload all grids (including locked in memory)
        });

    // Start the Remote Access port (acceptor) if enabled
    std::unique_ptr<AsyncAcceptor> raAcceptor;
    if (sConfigMgr->GetBoolDefault("Ra.Enable", false))
        raAcceptor.reset(StartRaSocketAcceptor(*ioContext));

    // Start soap serving thread if enabled
    std::shared_ptr<std::thread> soapThread;
    if (sConfigMgr->GetBoolDefault("SOAP.Enabled", false))
    {
        soapThread.reset(new std::thread(TCSoapThread, sConfigMgr->GetStringDefault("SOAP.IP", "127.0.0.1"), uint16(sConfigMgr->GetIntDefault("SOAP.Port", 7878))),
            [](std::thread *thr)
            {
                thr->join();
                delete thr;
            });
    }

    // Launch the worldserver listener socket
    uint16 worldPort = uint16(sWorld->getIntConfig(CONFIG_PORT_WORLD));
    std::string worldListener = sConfigMgr->GetStringDefault("BindIP", "0.0.0.0");

    int networkThreads = sConfigMgr->GetIntDefault("Network.Threads", 1);

    if (networkThreads <= 0)
    {
        LOG_ERROR("server.worldserver", "Network.Threads must be greater than 0");
        World::StopNow(ERROR_EXIT_CODE);
        return 1;
    }

    if (!sWorldSocketMgr.StartWorldNetwork(*ioContext, worldListener, worldPort, networkThreads))
    {
        LOG_ERROR("server.worldserver", "Failed to initialize network");
        World::StopNow(ERROR_EXIT_CODE);
        return 1;
    }

    std::shared_ptr<void> sWorldSocketMgrHandle(nullptr,
        [](void *)
        {
            sWorld->KickAll();         // save and kick all players
            sWorld->UpdateSessions(1); // real players unload required UpdateSessions call

            sWorldSocketMgr.StopNetwork();

            ///- Clean database before leaving
            ClearOnlineAccounts();
        });

    // Launch CliRunnable thread
    std::shared_ptr<std::thread> cliThread;
#if FC_PLATFORM == FC_PLATFORM_WINDOWS
    if (sConfigMgr->GetBoolDefault("Console.Enable", true) && (m_ServiceStatus == -1) /* need disable console in service mode*/)
#else
    if (sConfigMgr->GetBoolDefault("Console.Enable", true))
#endif
    {
        cliThread.reset(new std::thread(CliThread), &ShutdownCLIThread);
    }

    // Set server online (allow connecting now)
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = flag & ~%u, population = 0 WHERE id = '%u'", REALM_FLAG_OFFLINE, realm.Id.Realm);
    realm.PopulationLevel = 0.0f;
    realm.Flags = RealmFlags(realm.Flags & ~uint32(REALM_FLAG_OFFLINE));

    // Start the freeze check callback cycle in 5 seconds (cycle itself is 1 sec)
    std::shared_ptr<FreezeDetector> freezeDetector;
    if (int coreStuckTime = sConfigMgr->GetIntDefault("MaxCoreStuckTime", 0))
    {
        freezeDetector = std::make_shared<FreezeDetector>(*ioContext, coreStuckTime * 1000);
        FreezeDetector::Start(freezeDetector);
        LOG_INFO("server.worldserver", "Starting up anti-freeze thread (%u seconds max stuck time)...", coreStuckTime);
    }

    LOG_INFO("server.worldserver", "%s (worldserver-daemon) ready...", GitRevision::GetFullVersion());

    sScriptMgr->OnStartup();

    WorldUpdateLoop();

    // Shutdown starts here
    threadPool.reset();

    sLog->SetSynchronous();

    sScriptMgr->OnShutdown();

    // set server offline
    LoginDatabase.DirectPExecute("UPDATE realmlist SET flag = flag | %u WHERE id = '%d'", REALM_FLAG_OFFLINE, realm.Id.Realm);

    LOG_INFO("server.worldserver", "Halting process...");

    // 0 - normal shutdown
    // 1 - shutdown at error
    // 2 - restart command used, this code can be used by restarter for restart
    // Firelandsd

    return World::GetExitCode();
}

void ShutdownCLIThread(std::thread *cliThread)
{
    if (cliThread != nullptr)
    {
#if FC_PLATFORM_WINDOWS
        // First try to cancel any I/O in the CLI thread
        if (!CancelSynchronousIo(cliThread->native_handle()))
        {
            // if CancelSynchronousIo() fails, print the error and try with old way
            DWORD errorCode = GetLastError();
            LPSTR errorBuffer;

            DWORD formatReturnCode =
                FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, errorCode, 0, (LPTSTR)&errorBuffer, 0, nullptr);
            if (!formatReturnCode)
                errorBuffer = "Unknown error";

            LOG_DEBUG("server.worldserver", "Error cancelling I/O of CliThread, error code %u, detail: %s", uint32(errorCode), errorBuffer);

            if (!formatReturnCode)
                LocalFree(errorBuffer);

            // send keyboard input to safely unblock the CLI thread
            INPUT_RECORD b[4];
            HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
            b[0].EventType = KEY_EVENT;
            b[0].Event.KeyEvent.bKeyDown = TRUE;
            b[0].Event.KeyEvent.uChar.AsciiChar = 'X';
            b[0].Event.KeyEvent.wVirtualKeyCode = 'X';
            b[0].Event.KeyEvent.wRepeatCount = 1;

            b[1].EventType = KEY_EVENT;
            b[1].Event.KeyEvent.bKeyDown = FALSE;
            b[1].Event.KeyEvent.uChar.AsciiChar = 'X';
            b[1].Event.KeyEvent.wVirtualKeyCode = 'X';
            b[1].Event.KeyEvent.wRepeatCount = 1;

            b[2].EventType = KEY_EVENT;
            b[2].Event.KeyEvent.bKeyDown = TRUE;
            b[2].Event.KeyEvent.dwControlKeyState = 0;
            b[2].Event.KeyEvent.uChar.AsciiChar = '\r';
            b[2].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            b[2].Event.KeyEvent.wRepeatCount = 1;
            b[2].Event.KeyEvent.wVirtualScanCode = 0x1c;

            b[3].EventType = KEY_EVENT;
            b[3].Event.KeyEvent.bKeyDown = FALSE;
            b[3].Event.KeyEvent.dwControlKeyState = 0;
            b[3].Event.KeyEvent.uChar.AsciiChar = '\r';
            b[3].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            b[3].Event.KeyEvent.wVirtualScanCode = 0x1c;
            b[3].Event.KeyEvent.wRepeatCount = 1;
            DWORD numb;
            WriteConsoleInput(hStdIn, b, 4, &numb);
        }
#endif
        cliThread->join();
        delete cliThread;
    }
}

void WorldUpdateLoop()
{
    uint32 minUpdateDiff = uint32(sConfigMgr->GetIntDefault("MinWorldUpdateTime", 1));
    uint32 realCurrTime = 0;
    uint32 realPrevTime = getMSTime();

    ///- While we have not World::m_stopEvent, update the world
    while (!World::IsStopped())
    {
        ++World::m_worldLoopCounter;
        realCurrTime = getMSTime();

        uint32 diff = getMSTimeDiff(realPrevTime, realCurrTime);
        if (diff < minUpdateDiff)
        {
            // sleep until enough time passes that we can update all timers
            std::this_thread::sleep_for(Milliseconds(minUpdateDiff - diff));
            continue;
        }

        sWorld->Update(diff);
        realPrevTime = realCurrTime;

#if FC_PLATFORM == FC_PLATFORM_WINDOWS
        if (m_ServiceStatus == 0)
            World::StopNow(SHUTDOWN_EXIT_CODE);

        while (m_ServiceStatus == 2)
            Sleep(1000);
#endif
    }
}

void SignalHandler(boost::system::error_code const &error, int /*signalNumber*/)
{
    if (!error)
        World::StopNow(SHUTDOWN_EXIT_CODE);
}

void FreezeDetector::Handler(std::weak_ptr<FreezeDetector> freezeDetectorRef, boost::system::error_code const &error)
{
    if (!error)
    {
        if (std::shared_ptr<FreezeDetector> freezeDetector = freezeDetectorRef.lock())
        {
            uint32 curtime = getMSTime();

            uint32 worldLoopCounter = World::m_worldLoopCounter;
            if (freezeDetector->_worldLoopCounter != worldLoopCounter)
            {
                freezeDetector->_lastChangeMsTime = curtime;
                freezeDetector->_worldLoopCounter = worldLoopCounter;
            }
            // possible freeze
            else if (getMSTimeDiff(freezeDetector->_lastChangeMsTime, curtime) > freezeDetector->_maxCoreStuckTimeInMs)
            {
                LOG_ERROR("server.worldserver", "World Thread hangs, kicking out server!");
                ABORT();
            }

            freezeDetector->_timer.expires_from_now(boost::posix_time::seconds(1));
            freezeDetector->_timer.async_wait(std::bind(&FreezeDetector::Handler, freezeDetectorRef, std::placeholders::_1));
        }
    }
}

AsyncAcceptor *StartRaSocketAcceptor(Firelands::Asio::IoContext &ioContext)
{
    uint16 raPort = uint16(sConfigMgr->GetIntDefault("Ra.Port", 3443));
    std::string raListener = sConfigMgr->GetStringDefault("Ra.IP", "0.0.0.0");

    AsyncAcceptor *acceptor = new AsyncAcceptor(ioContext, raListener, raPort);
    if (!acceptor->Bind())
    {
        LOG_ERROR("server.worldserver", "Failed to bind RA socket acceptor");
        delete acceptor;
        return nullptr;
    }

    acceptor->AsyncAccept<RASession>();
    return acceptor;
}

bool LoadRealmInfo(Firelands::Asio::IoContext &ioContext)
{
    QueryResult result = LoginDatabase.PQuery("SELECT id, name, address, localAddress, localSubnetMask, port, icon, "
                                              "flag, timezone, allowedSecurityLevel, population, gamebuild FROM "
                                              "realmlist WHERE id = %u",
        realm.Id.Realm);
    if (!result)
        return false;

    Firelands::Asio::Resolver resolver(ioContext);

    Field *fields = result->Fetch();
    realm.Name = fields[1].GetString();
    Optional<boost::asio::ip::tcp::endpoint> externalAddress = resolver.Resolve(boost::asio::ip::tcp::v4(), fields[2].GetString(), "");
    if (!externalAddress)
    {
        LOG_ERROR("server.worldserver", "Could not resolve address %s", fields[2].GetString().c_str());
        return false;
    }

    realm.ExternalAddress = Firelands::make_unique<boost::asio::ip::address>(externalAddress->address());

    Optional<boost::asio::ip::tcp::endpoint> localAddress = resolver.Resolve(boost::asio::ip::tcp::v4(), fields[3].GetString(), "");
    if (!localAddress)
    {
        LOG_ERROR("server.worldserver", "Could not resolve address %s", fields[3].GetString().c_str());
        return false;
    }

    realm.LocalAddress = Firelands::make_unique<boost::asio::ip::address>(localAddress->address());

    Optional<boost::asio::ip::tcp::endpoint> localSubmask = resolver.Resolve(boost::asio::ip::tcp::v4(), fields[4].GetString(), "");
    if (!localSubmask)
    {
        LOG_ERROR("server.worldserver", "Could not resolve address %s", fields[4].GetString().c_str());
        return false;
    }

    realm.LocalSubnetMask = Firelands::make_unique<boost::asio::ip::address>(localSubmask->address());

    realm.Port = fields[5].GetUInt16();
    realm.Type = fields[6].GetUInt8();
    realm.Flags = RealmFlags(fields[7].GetUInt8());
    realm.Timezone = fields[8].GetUInt8();
    realm.AllowedSecurityLevel = AccountTypes(fields[9].GetUInt8());
    realm.PopulationLevel = fields[10].GetFloat();
    realm.Build = fields[11].GetUInt32();
    return true;
}

/// Initialize connection to the databases
bool StartDB()
{
    MySQL::Library_Init();

    // Load databases
    DatabaseLoader loader("server.worldserver", DatabaseLoader::DATABASE_NONE, FC_MODULES_LIST);
    loader.AddDatabase(LoginDatabase, "Login").AddDatabase(CharacterDatabase, "Character").AddDatabase(WorldDatabase, "World").AddDatabase(HotfixDatabase, "Hotfix");

    if (!loader.Load())
        return false;

    ///- Get the realm Id from the configuration file
    realm.Id.Realm = sConfigMgr->GetIntDefault("RealmID", 0);
    if (!realm.Id.Realm)
    {
        LOG_ERROR("server.worldserver", "Realm ID not defined in configuration file");
        return false;
    }

    QueryResult realmIdQuery = LoginDatabase.PQuery("SELECT `Region`,`Battlegroup` FROM `realmlist` WHERE `id`=%u", realm.Id.Realm);
    if (!realmIdQuery)
    {
        LOG_ERROR("server.worldserver", "Realm id %u not defined in realmlist table", realm.Id.Realm);
        return false;
    }

    realm.Id.Region = (*realmIdQuery)[0].GetUInt8();
    realm.Id.Site = (*realmIdQuery)[1].GetUInt8();

    LOG_INFO("server.worldserver", "Realm running as realm ID %u region %u battlegroup %u", realm.Id.Realm, uint32(realm.Id.Region), uint32(realm.Id.Site));

    ///- Clean the database before starting
    ClearOnlineAccounts();

    ///- Insert version info into DB
    WorldDatabase.PExecute("UPDATE version SET core_version = '%s', core_revision = '%s'", GitRevision::GetFullVersion(),
        GitRevision::GetHash()); // One-time query

    sWorld->LoadDBVersion();

    LOG_INFO("server.worldserver", "Using World DB: %s", sWorld->GetDBVersion());
    return true;
}

void StopDB()
{
    CharacterDatabase.Close();
    WorldDatabase.Close();
    LoginDatabase.Close();
    HotfixDatabase.Close();

    MySQL::Library_End();
}

/// Clear 'online' status for all accounts with characters in this realm
void ClearOnlineAccounts()
{
    // Reset online status for all accounts with characters on the current realm
    LoginDatabase.DirectPExecute("UPDATE account SET online = 0 WHERE online > 0 AND id IN (SELECT acctid "
                                 "FROM realmcharacters WHERE realmid = %d)",
        realm.Id.Realm);

    // Reset online status for all characters
    CharacterDatabase.DirectExecute("UPDATE characters SET online = 0 WHERE online <> 0");

    // Battleground instance ids reset at server restart
    CharacterDatabase.DirectExecute("UPDATE character_battleground_data SET instanceId = 0");
}

/// @}

variables_map GetConsoleArguments(int argc, char **argv, fs::path &configFile, std::string &configService)
{
    // Silences warning about configService not be used if the OS is not Windows
    (void)configService;

    options_description all("Allowed options");
    all.add_options()("help,h", "print usage message")("version,v", "print version build info")(
        "config,c", value<fs::path>(&configFile)->default_value(fs::absolute(_FIRELANDS_CORE_CONFIG)), "use <arg> as configuration file")("update-databases-only,u", "updates databases only");
#if FC_PLATFORM == FC_PLATFORM_WINDOWS
    options_description win("Windows platform specific options");
    win.add_options()("service,s", value<std::string>(&configService)->default_value(""), "Windows service options: [install | uninstall]");

    all.add(win);
#endif
    variables_map vm;
    try
    {
        store(command_line_parser(argc, argv).options(all).allow_unregistered().run(), vm);
        notify(vm);
    }
    catch (std::exception &e)
    {
        std::cerr << e.what() << "\n";
    }

    if (vm.count("help"))
    {
        std::cout << all << "\n";
    }
    else if (vm.count("version"))
    {
        std::cout << GitRevision::GetFullVersion() << "\n";
    }

    return vm;
}
