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

/// \addtogroup realmd Realm Daemon
/// @{
/// \file

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "RealmList.h"

#include "Config/Config.h"
#include "Log.h"
#include "AuthSocket.h"
#include "SystemConfig.h"
#include "revision.h"
#include "revision_sql.h"
#include "Util.h"
#include "Network/Listener.hpp"

#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem/operations.hpp>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>

#include <iostream>
#include <string>
#include <chrono>
#include <thread>

using boost::asio::ip::tcp;
using namespace boost::program_options;

#ifdef _WIN32
#include "ServiceWin32.h"
char serviceName[] = "realmd";
char serviceLongName[] = "Authenication Service";
char serviceDescription[] = "World of Warcraft Authentication Service";
/*
 * -1 - not in service mode
 *  0 - stopped
 *  1 - running
 *  2 - paused
 */
int m_ServiceStatus = -1;

boost::asio::deadline_timer* _serviceStatusWatchTimer;
void ServiceStatusWatcher(boost::system::error_code const& error);
#endif

bool StartDB();
void SignalHandler(const boost::system::error_code& error, int signalNumber);
void KeepDatabaseAliveHandler(const boost::system::error_code& error);

boost::asio::io_service* _ioService;
boost::asio::deadline_timer* _dbPingTimer;
uint32 _dbPingInterval;

DatabaseType LoginDatabase;                                 ///< Accessor to the realm server database

/// Launch the realm server
int main(int argc, char *argv[])
{
    std::string configFile = _REALMD_CONFIG;
    if (!sConfig.SetSource(configFile))
    {
        sLog.outError("Could not find configuration file %s.", configFile.c_str());
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

    sLog.Initialize();

    sLog.outString("realm deamon");
    sLog.outString("<Ctrl-C> to stop.\n");
    sLog.outString("Using configuration file %s.", configFile.c_str());
    sLog.outString("Using SSL Version: %s (library: %s", OPENSSL_VERSION_TEXT, SSLeay_version(SSLEAY_VERSION));
    sLog.outString("Using Boost Version: %i.%i.%i", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);

    /// realmd PID file creation
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

    ///- Initialize the database connection
    if (!StartDB())
    {
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

    _ioService = new boost::asio::io_service();

    ///- Get the list of realms for the server
    sRealmList->Initialize(*_ioService, sConfig.GetIntDefault("RealmsStateUpdateDelay", 20));
    if (sRealmList->size() == 0)
    {
        sLog.outError("No valid realms specified.");
        Log::WaitBeforeContinueIfNeed();
        delete _ioService;
        return 1;
    }

    // cleanup query
    // set expired bans to inactive
    LoginDatabase.BeginTransaction();
    LoginDatabase.Execute("UPDATE account_banned SET active = 0 WHERE unbandate <= UNIX_TIMESTAMP() AND unbandate <> bandate");
    LoginDatabase.Execute("DELETE FROM ip_banned WHERE unbandate <= UNIX_TIMESTAMP() AND unbandate <> bandate");
    LoginDatabase.CommitTransaction();

    int32 rmport = sConfig.GetIntDefault("RealmServerPort", 3724);
    if (rmport < 0 || rmport > 0xFFFF)
    {
        sLog.outError("Specified port out of allowed range (1-65535");
        delete _ioService;
        return 1;
    }

    // Dead string of code. Need to update AuthSocket for this to work.
    std::string bind_ip = sConfig.GetStringDefault("BindIP", "0.0.0.0");

    // FIXME - more intelligent selection of thread count is needed here.  config option?
    MaNGOS::Listener<AuthSocket> listener(rmport, 1);

    boost::asio::signal_set signals(*_ioService, SIGINT, SIGTERM);
    #ifdef _WIN32
    signals.add(SIGBREAK);
    #endif

    signals.async_wait(SignalHandler);

    ///- Handle affinity for multiple processors and process priority on Windows
#ifdef _WIN32 /// - Needs rework
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
                    sLog.outError("Processors marked in UseProcessors bitmask (hex) %x not accessible for realmd. Accessible processors bitmask (hex): %x", Aff, appAff);
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

        if (Prio)
        {
            if (SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS))
                sLog.outString("realmd process priority class set to HIGH");
            else
                sLog.outError("Can't set realmd process priority class.");
            sLog.outString();
        }
    }
#endif

    // server has started up successfully => enable async DB requests
    LoginDatabase.AllowAsyncTransactions();

    // Enabled a timed callback for handling the database keep alive ping
    _dbPingInterval = sConfig.GetIntDefault("MaxPingTime", 30);
    _dbPingTimer = new boost::asio::deadline_timer(*_ioService);
    _dbPingTimer->expires_from_now(boost::posix_time::minutes(_dbPingInterval));
    _dbPingTimer->async_wait(KeepDatabaseAliveHandler);

    #ifdef _WIN32
    if (m_ServiceStatus != -1)
    {
        _serviceStatusWatchTimer = new boost::asio::deadline_timer(*_ioService);
        _serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
        _serviceStatusWatchTimer->async_wait(ServiceStatusWatcher);
    }
    #endif

    _ioService->run();

    _dbPingTimer->cancel();

    ///- Wait for the delay thread to exit
    LoginDatabase.HaltDelayThread();

    sLog.outString("Halting process...");

    signals.cancel();

    delete _dbPingTimer;
    delete _ioService;
    return 0;
}

/// Initialize connection to the database
bool StartDB()
{
    std::string dbstring = sConfig.GetStringDefault("LoginDatabaseInfo");
    if (dbstring.empty())
    {
        sLog.outError("Database not specified");
        return false;
    }

    sLog.outString("Login Database total connections: %i", 1 + 1);

    if (!LoginDatabase.Initialize(dbstring.c_str()))
    {
        sLog.outError("Cannot connect to database");
        return false;
    }

    if (!LoginDatabase.CheckRequiredField("realmd_db_version", REVISION_DB_REALMD))
    {
        ///- Wait for already started DB delay threads to end
        LoginDatabase.HaltDelayThread();
        return false;
    }

    return true;
}

void SignalHandler(const boost::system::error_code& error, int /*signalNumber*/)
{
    if (!error)
        _ioService->stop();
}

void KeepDatabaseAliveHandler(const boost::system::error_code& error)
{
    if (!error)
    {
        sLog.outString("Ping MySQL to keep connection alive");
        LoginDatabase.Ping();

        _dbPingTimer->expires_from_now(boost::posix_time::minutes(_dbPingInterval));
        _dbPingTimer->async_wait(KeepDatabaseAliveHandler);
    }
}

#ifdef _WIN32
void ServiceStatusWatcher(boost::system::error_code const& error)
{
    if (!error)
    {
        if (m_ServiceStatus == 0)
        {
            _ioService->stop();
            delete _serviceStatusWatchTimer;
        }
        else
        {
            _serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
            _serviceStatusWatchTimer->async_wait(ServiceStatusWatcher);
        }
    }
}
#endif
