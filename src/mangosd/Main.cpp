/*
 * This file is part of the Firestorm Freelance Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/// \addtogroup mangosd Mangos Daemon
/// @{
/// \file

#include "Common.h"
#include "Server/WorldSocket.h"
#include "WorldRunnable.h"
#include "World/World.h"
#include "Log.h"
#include "Timer.h"
#include "SystemConfig.h"
#include "CliRunnable.h"
#include "RASocket.h"
#include "Util.h"
#include "revision_sql.h"
#include "MaNGOSsoap.h"
#include "Mails/MassMailMgr.h"
#include "Server/DBCStores.h"

#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Policies/Singleton.h"
#include "Network/Listener.hpp"
#include "Network/Socket.hpp"

#include "AuctionHouseBot/AuctionHouseBot.h"

#include <openssl/opensslv.h>
#include <openssl/crypto.h>

#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>

#include <iostream>
#include <string>

#ifdef _WIN32
#include "ServiceWin32.h"
char serviceName[] = "mangosd";
char serviceLongName[] = "Worldserver Service";
char serviceDescription[] = "World of Warcraft Worldserver Service";
/*
 * -1 - not in service mode
 *  0 - stopped
 *  1 - running
 *  2 - paused
 */
int m_ServiceStatus = -1;
#endif

boost::asio::io_service* _ioService;

DatabaseType WorldDatabase;                                 ///< Accessor to the world database
DatabaseType CharacterDatabase;                             ///< Accessor to the character database
DatabaseType LoginDatabase;                                 ///< Accessor to the realm/login database

uint32 realmID;                                             ///< Id of the realm

bool StartDB();
void HookSignals();
void UnhookSignals();
static void OnSignal(int s);
void clearOnlineAccounts();

/// Launch the mangos server
int main(int argc, char * argv[])
{
    std::string auctionBotConfig;
    sAuctionBotConfig.SetConfigFileName(auctionBotConfig);

    std::string configFile = _MANGOSD_CONFIG;
    if (!sConfig.SetSource(configFile))
    {
        sLog.outError("Could not find configuration file %s.", configFile.c_str());
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

    sLog.outString("World Server");
    sLog.outString("<Ctrl-C> to stop.\n");
    sLog.outString("Using configuration file %s.", configFile.c_str());
    sLog.outString("Using SSL Version: %s (library: %s", OPENSSL_VERSION_TEXT, SSLeay_version(SSLEAY_VERSION));
    sLog.outString("Using Boost Version: %i.%i.%i", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);

    /// worldd PID file creation
    std::string pidfile = sConfig.GetStringDefault("PidFile", "");
    if (!pidfile.empty())
    {
        if (uint32 pid = CreatePIDFile(pidfile))
            sLog.outString("Daemon PID: %u\n", pid);
        else
        {
            sLog.outError("Cannot create PID file %s.\n", pidfile.c_str());
            Log::WaitBeforeContinueIfNeed();
            return 1;
        }
    }

    ///- Set server offline in realmlist
    LoginDatabase.DirectPExecute("UPDATE realmlist SET realmflags = realmflags | %u WHERE id = '%u'", REALM_FLAG_OFFLINE, realmID);

    ///- Start the databases
    if (!StartDB())
    {
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

    ///- Initialize the World
    sWorld.SetInitialWorldSettings();

    // server loaded successfully => enable async DB requests
    // this is done to forbid any async transactions during server startup!
    CharacterDatabase.AllowAsyncTransactions();
    WorldDatabase.AllowAsyncTransactions();
    LoginDatabase.AllowAsyncTransactions();

    ///- Catch termination signals
    HookSignals();

    ///- Launch WorldRunnable thread
    MaNGOS::Thread world_thread(new WorldRunnable);
    world_thread.setPriority(MaNGOS::Priority_Highest);

    // set realmbuilds depend on mangosd expected builds, and set server online
    std::string builds = AcceptableClientBuildsListStr();
    LoginDatabase.escape_string(builds);
    LoginDatabase.DirectPExecute("UPDATE realmlist SET realmflags = realmflags & ~%u, population = 0, realmbuilds = '%s'  WHERE id = '%u'", REALM_FLAG_OFFLINE, builds.c_str(), realmID);

    MaNGOS::Thread* cliThread = nullptr;
#ifdef _WIN32
    if (sConfig.GetBoolDefault("Console.Enable", true) && (m_ServiceStatus == -1)/* need disable console in service mode*/)
#else
    if (sConfig.GetBoolDefault("Console.Enable", true))
#endif
    {
        ///- Launch CliRunnable thread
        cliThread = new MaNGOS::Thread(new CliRunnable);
    }

    ///- Handle affinity for multiple processors and process priority on Windows
#ifdef _WIN32
    {
        HANDLE hProcess = GetCurrentProcess();

        uint32 Aff = sConfig.GetIntDefault("UseProcessors", 0);
        if (Aff > 0)
        {
            ULONG_PTR appAff;
            ULONG_PTR sysAff;

            if (GetProcessAffinityMask(hProcess, &appAff, &sysAff))
            {
                ULONG_PTR curAff = Aff & appAff;            // remove non accessible processors

                if (!curAff)
                {
                    sLog.outError("Processors marked in UseProcessors bitmask (hex) %x not accessible for mangosd. Accessible processors bitmask (hex): %x", Aff, appAff);
                }
                else
                {
                    if (SetProcessAffinityMask(hProcess, curAff))
                        sLog.outString("Using processors (bitmask, hex): %x", curAff);
                    else
                        sLog.outError("Can't set used processors (hex): %x", curAff);
                }
            }
            sLog.outString();
        }

        bool Prio = sConfig.GetBoolDefault("ProcessPriority", false);

        //        if(Prio && (m_ServiceStatus == -1)/* need set to default process priority class in service mode*/)
        if (Prio)
        {
            if (SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS))
                sLog.outString("mangosd process priority class set to HIGH");
            else
                sLog.outError("Can't set mangosd process priority class.");
            sLog.outString();
        }
    }
#endif

    //auto const listenIP = sConfig.GetStringDefault("BindIP", "0.0.0.0");
    MaNGOS::Listener<WorldSocket> listener(sWorld.getConfig(CONFIG_UINT32_PORT_WORLD), 8);

    std::unique_ptr<MaNGOS::Listener<RASocket>> raListener;
    if (sConfig.GetBoolDefault("Ra.Enable", false))
        raListener.reset(new MaNGOS::Listener<RASocket>(sConfig.GetIntDefault("Ra.Port", 3443), 1));

    std::unique_ptr<SOAPThread> soapThread;
    if (sConfig.GetBoolDefault("SOAP.Enabled", false))
        soapThread.reset(new SOAPThread("0.0.0.0", sConfig.GetIntDefault("SOAP.Port", 7878)));

    // wait for shut down and then let things go out of scope to close them down
    while (!World::IsStopped())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    ///- Set server offline in realmlist
    LoginDatabase.DirectPExecute("UPDATE realmlist SET realmflags = realmflags | %u WHERE id = '%u'", REALM_FLAG_OFFLINE, realmID);

    ///- Remove signal handling before leaving
    UnhookSignals();

    // when the main thread closes the singletons get unloaded
    // since worldrunnable uses them, it will crash if unloaded after master
    world_thread.wait();

    ///- Clean account database before leaving
    clearOnlineAccounts();

    // send all still queued mass mails (before DB connections shutdown)
    sMassMailMgr.Update(true);

    ///- Wait for DB delay threads to end
    CharacterDatabase.HaltDelayThread();
    WorldDatabase.HaltDelayThread();
    LoginDatabase.HaltDelayThread();

    sLog.outString("Halting process...");

    if (cliThread)
    {
#ifdef _WIN32

        // this only way to terminate CLI thread exist at Win32 (alt. way exist only in Windows Vista API)
        //_exit(1);
        // send keyboard input to safely unblock the CLI thread
        INPUT_RECORD b[5];
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
        BOOL ret = WriteConsoleInput(hStdIn, b, 4, &numb);

        cliThread->wait();
#else
        cliThread->destroy();
#endif
        delete cliThread;
    }

    ///- Exit the process with specified return value
    return World::GetExitCode();
}

/// Initialize connection to the databases
bool StartDB()
{
    ///- Get world database info from configuration file
    std::string dbstring = sConfig.GetStringDefault("WorldDatabaseInfo");
    int nConnections = sConfig.GetIntDefault("WorldDatabaseConnections", 1);
    if (dbstring.empty())
    {
        sLog.outError("Database not specified in configuration file");
        return false;
    }
    sLog.outString("World Database total connections: %i", nConnections + 1);

    ///- Initialise the world database
    if (!WorldDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("Cannot connect to world database %s", dbstring.c_str());
        return false;
    }

    if (!WorldDatabase.CheckRequiredField("db_version", REVISION_DB_MANGOS))
    {
        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        return false;
    }

    dbstring = sConfig.GetStringDefault("CharacterDatabaseInfo");
    nConnections = sConfig.GetIntDefault("CharacterDatabaseConnections", 1);
    if (dbstring.empty())
    {
        sLog.outError("Character Database not specified in configuration file");

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        return false;
    }
    sLog.outString("Character Database total connections: %i", nConnections + 1);

    ///- Initialise the Character database
    if (!CharacterDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("Cannot connect to Character database %s", dbstring.c_str());

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        return false;
    }

    if (!CharacterDatabase.CheckRequiredField("character_db_version", REVISION_DB_CHARACTERS))
    {
        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        return false;
    }

    ///- Get login database info from configuration file
    dbstring = sConfig.GetStringDefault("LoginDatabaseInfo");
    nConnections = sConfig.GetIntDefault("LoginDatabaseConnections", 1);
    if (dbstring.empty())
    {
        sLog.outError("Login database not specified in configuration file");

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        return false;
    }

    ///- Initialise the login database
    sLog.outString("Login Database total connections: %i", nConnections + 1);
    if (!LoginDatabase.Initialize(dbstring.c_str(), nConnections))
    {
        sLog.outError("Cannot connect to login database %s", dbstring.c_str());

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        return false;
    }

    if (!LoginDatabase.CheckRequiredField("realmd_db_version", REVISION_DB_REALMD))
    {
        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        LoginDatabase.HaltDelayThread();
        return false;
    }

    sLog.outString();

    ///- Get the realm Id from the configuration file
    realmID = sConfig.GetIntDefault("RealmID", 0);
    if (!realmID)
    {
        sLog.outError("Realm ID not defined in configuration file");

        ///- Wait for already started DB delay threads to end
        WorldDatabase.HaltDelayThread();
        CharacterDatabase.HaltDelayThread();
        LoginDatabase.HaltDelayThread();
        return false;
    }

    sLog.outString("Realm running as realm ID %d", realmID);
    sLog.outString();

    ///- Clean the database before starting
    clearOnlineAccounts();

    sWorld.LoadDBVersion();

    sLog.outString("Using World DB: %s", sWorld.GetDBVersion());
    sLog.outString("Using creature EventAI: %s", sWorld.GetCreatureEventAIVersion());
    sLog.outString();
    return true;
}


/// Clear 'online' status for all accounts with characters in this realm
void clearOnlineAccounts()
{
    // Cleanup online status for characters hosted at current realm
    /// \todo Only accounts with characters logged on *this* realm should have online status reset. Move the online column from 'account' to 'realmcharacters'?
    LoginDatabase.PExecute("UPDATE account SET active_realm_id = 0 WHERE active_realm_id = '%u'", realmID);

    CharacterDatabase.Execute("UPDATE characters SET online = 0 WHERE online <> 0");

    // Battleground instance ids reset at server restart
    CharacterDatabase.Execute("UPDATE character_battleground_data SET instance_id = 0");
}

/// Handle termination signals
void OnSignal(int s)
{
    switch (s)
    {
    case SIGINT:
        World::StopNow(RESTART_EXIT_CODE);
        break;
    case SIGTERM:
#ifdef _WIN32
    case SIGBREAK:
#endif
        World::StopNow(SHUTDOWN_EXIT_CODE);
        break;
    }

    // give a 30 sec timeout in case of Master cannot finish properly
    int32 timeOut = 200;
    while (timeOut > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        --timeOut;
    }

    signal(s, OnSignal);
}

/// Define hook '_OnSignal' for all termination signals
void HookSignals()
{
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
#ifdef _WIN32
    signal(SIGBREAK, OnSignal);
#endif
}

/// Unhook the signals before leaving
void UnhookSignals()
{
    signal(SIGINT, 0);
    signal(SIGTERM, 0);
#ifdef _WIN32
    signal(SIGBREAK, 0);
#endif
}
