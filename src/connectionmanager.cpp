/*
 * Copyright (c) 2011, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "shellapi.h"
#include "config.h"
#include "psiclient.h"
#include "connectionmanager.h"
#include "httpsrequest.h"
#include "webbrowser.h"
#include "embeddedvalues.h"
#include "sshconnection.h"
#include <algorithm>
#include <sstream>


// Upgrade process posts a Quit message
extern HWND g_hWnd;


ConnectionManager::ConnectionManager(void) :
    m_state(CONNECTION_MANAGER_STATE_STOPPED),
    m_userSignalledStop(false),
    m_sshConnection(m_userSignalledStop),
    m_thread(0)
{
    m_mutex = CreateMutex(NULL, FALSE, 0);
}

ConnectionManager::~ConnectionManager(void)
{
    Stop();
    CloseHandle(m_mutex);
}

void ConnectionManager::OpenHomePages(void)
{
    AutoMUTEX lock(m_mutex);
    
    OpenBrowser(m_currentSessionInfo.GetHomepages());
}

void ConnectionManager::Toggle()
{
    // NOTE: no lock, to allow thread to access object

    if (m_state == CONNECTION_MANAGER_STATE_STOPPED)
    {
        Start();
    }
    else
    {
        Stop();
    }
}

void ConnectionManager::Stop(void)
{
    // NOTE: no lock, to allow thread to access object

    // The assumption is that signalling stop will cause any current operations to
    // stop (such as making HTTPS requests, or establishing a connection), and
    // cause the connection to hang up if it is connected.
    // While a connection is active, there is a thread running waiting for the
    // connection to terminate.

    // Cancel flag is also termination flag
    m_userSignalledStop = true;

    // Wait for thread to exit (otherwise can get access violation when app terminates)
    if (m_thread)
    {
        WaitForSingleObject(m_thread, INFINITE);
        m_thread = 0;
    }
}

void ConnectionManager::Start(void)
{
    // Call Stop to cleanup in case thread failed on last Start attempt
    Stop();

    AutoMUTEX lock(m_mutex);

    m_userSignalledStop = false;

    if (m_state != CONNECTION_MANAGER_STATE_STOPPED || m_thread != 0)
    {
        my_print(false, _T("Invalid connection manager state in Start (%d)"), m_state);
        return;
    }

    SetState(CONNECTION_MANAGER_STATE_STARTING);

    if (!(m_thread = CreateThread(0, 0, ConnectionManagerStartThread, (void*)this, 0, 0)))
    {
        my_print(false, _T("Start: CreateThread failed (%d)"), GetLastError());

        SetState(CONNECTION_MANAGER_STATE_STOPPED);
    }
}

void ConnectionManager::DoVPNConnection(
        ConnectionManager* manager,
        const ServerEntry& serverEntry)
{
    //
    // Minimum version check for VPN
    // - L2TP/IPSec/PSK not supported on Windows 2000
    // - (TODO: once we add SSH, fail over to SSH)
    //
    
    OSVERSIONINFO versionInfo;
    versionInfo.dwOSVersionInfoSize = sizeof(versionInfo);
    if (!GetVersionEx(&versionInfo) ||
            versionInfo.dwMajorVersion < 5 ||
            (versionInfo.dwMajorVersion == 5 && versionInfo.dwMinorVersion == 0))
    {
        my_print(false, _T("Windows XP or greater required"));
        throw Abort();
    }
    
    //
    // Check VPN services and fix if required/possible
    //
    
    // Note: we proceed even if the call fails. Testing is inconsistent -- don't
    // always need all tweaks to connect.
    TweakVPN();
    
    //
    // Start VPN connection
    //
    
    manager->VPNEstablish();
    
    //
    // Monitor VPN connection and wait for CONNECTED or FAILED
    //
    
    manager->WaitForVPNConnectionStateToChangeFrom(VPN_CONNECTION_STATE_STARTING);
    
    if (VPN_CONNECTION_STATE_CONNECTED != manager->GetVPNConnectionState())
    {
        // Note: WaitForVPNConnectionStateToChangeFrom throws Abort if user
        // cancelled, so if we're here it's a FAILED case.
    
        // Report error code to server for logging/trouble-shooting.
        // The request line includes the last VPN error code.
        
        tstring requestPath = manager->GetFailedRequestPath();
    
        string response;
        HTTPSRequest httpsRequest;
        if (!httpsRequest.GetRequest(
                            manager->GetUserSignalledStop(),
                            NarrowToTString(serverEntry.serverAddress).c_str(),
                            serverEntry.webServerPort,
                            serverEntry.webServerCertificate,
                            requestPath.c_str(),
                            response))
        {
            // Ignore failure
        }
    
        // Wait between 1 and 5 seconds before retrying. This is a quick
        // fix to deal with the following problem: when a client can
        // make an HTTPS connection but not a VPN connection, it ends
        // up spamming "handshake" requests, resulting in PSK race conditions
        // with other clients that are trying to connect. This is starving
        // clients that are able to establish the VPN connection.
        // TODO: a more optimal solution would only wait when re-trying
        // a server where this condition (HTTPS ok, VPN failed) previously
        // occurred.
        Sleep(1000 + rand()%4000);
    
        throw TryNextServer();
    }
    
    manager->SetState(CONNECTION_MANAGER_STATE_CONNECTED_VPN);
    
    //
    // Patch DNS bug on Windowx XP; and flush DNS
    // to ensure domains are resolved with VPN's DNS server
    //
    
    // Note: we proceed even if the call fails. This means some domains
    // may not resolve properly.
    TweakDNS();
    
    //
    // Open home pages in browser
    //
    
    manager->OpenHomePages();
    
    //
    // "Connected" HTTPS request for server stats (not critical to succeed)
    //
    
    tstring connectedRequestPath = manager->GetConnectRequestPath();
    
    // There's no content in the response. Also, failure is ignored since
    // it just means the server didn't log a stat.
    
    string response;
    HTTPSRequest httpsRequest;
    if (!httpsRequest.GetRequest(
                        manager->GetUserSignalledStop(),
                        NarrowToTString(serverEntry.serverAddress).c_str(),
                        serverEntry.webServerPort,
                        serverEntry.webServerCertificate,
                        connectedRequestPath.c_str(),
                        response))
    {
        // Ignore failure
    }
    
    //
    // Wait for VPN connection to stop (or fail) -- set ConnectionManager state accordingly (used by UI)
    //
    
    manager->WaitForVPNConnectionStateToChangeFrom(VPN_CONNECTION_STATE_CONNECTED);
    
    manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);
}

void ConnectionManager::DoSSHConnection(ConnectionManager* manager)
{
    //
    // Establish SSH connection
    //

    // TEMP
    tstring hostKey = _T("<base64>==");
    if (!manager->SSHConnect(_T("1.1.1.1"),_T("22"),hostKey,_T("psiphonv"),_T("<password>"))
        || !manager->SSHWaitForConnected())
    {
        if (manager->GetUserSignalledStop())
        {
            throw Abort();
        }
        throw TryNextServer();
    }

    manager->SetState(CONNECTION_MANAGER_STATE_CONNECTED_SSH);

    //
    // Open home pages in browser
    //
   
    manager->OpenHomePages();    

    //
    // Wait for SSH connection to stop (or fail)
    //

    // Note: doesn't throw abort on user cancel, but it all works out the same
    manager->SSHWaitAndDisconnect();
    
    manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);
}

DWORD WINAPI ConnectionManager::ConnectionManagerStartThread(void* data)
{
    ConnectionManager* manager = (ConnectionManager*)data;

    // Loop through server list, attempting to connect.
    //
    // Connect sequence:
    //
    // - Make Handshake HTTPS request
    // - Perform download HTTPS request and upgrade, if applicable
    // - Try VPN:
    // -- Create and dial VPN connection
    // -- Tweak VPN system settings if required
    // -- Wait for VPN connection to succeed or fail
    // -- Flush DNS and fix settings if required
    // - If VPN failed:
    // -- Create SSH connection
    // -- Wait for SSH connection to succeed or fail
    // - If a connection type succeeded:
    // -- Launch home pages (failure is acceptable)
    // -- Make Connected HTTPS request (failure is acceptable)
    // -- Wait for connection to stop
    //
    // When handshake and all connection types fail, the
    // server is marked as failed in the local server list and
    // the next server from the list is selected and retried.
    //
    // All operations may be interrupted by user cancel.
    //
    // NOTE: this function doesn't hold the ConnectionManager
    // object lock to allow for cancel etc.

    while (true) // Try servers loop
    {
        try
        {
            //
            // Handshake HTTPS request
            //

            ServerEntry serverEntry;
            tstring handshakeRequestPath;
            string handshakeResponse;

            // Send list of known server IP addresses (used for stats logging on the server)

            manager->LoadNextServer(
                            serverEntry,
                            handshakeRequestPath);

            HTTPSRequest httpsRequest;
            if (!httpsRequest.GetRequest(
                                manager->GetUserSignalledStop(),
                                NarrowToTString(serverEntry.serverAddress).c_str(),
                                serverEntry.webServerPort,
                                serverEntry.webServerCertificate,
                                handshakeRequestPath.c_str(),
                                handshakeResponse))
            {
                if (manager->GetUserSignalledStop())
                {
                    throw Abort();
                }
                else
                {
                    throw TryNextServer();
                }
            }

            manager->HandleHandshakeResponse(handshakeResponse.c_str());

            //
            // Upgrade
            //

            // Upgrade now if handshake notified of new version
            tstring downloadRequestPath;
            string downloadResponse;
            if (manager->RequireUpgrade(downloadRequestPath))
            {
                // Download new binary

                if (!httpsRequest.GetRequest(
                            manager->GetUserSignalledStop(),
                            NarrowToTString(serverEntry.serverAddress).c_str(),
                            serverEntry.webServerPort,
                            serverEntry.webServerCertificate,
                            downloadRequestPath.c_str(),
                            downloadResponse))
                {
                    if (manager->GetUserSignalledStop())
                    {
                        throw Abort();
                    }
                    // else fall through to Establish()

                    // If the download failed, we simply proceed with the connection.
                    // Rationale:
                    // - The server is (and hopefully will remain) backwards compatible.
                    // - The failure is likely a configuration one, as the handshake worked.
                    // - A configuration failure could be common across all servers, so the
                    //   client will never connect.
                    // - Fail-over exposes new server IPs to hostile networks, so we don't
                    //   like doing it in the case where we know the handshake already succeeded.
                }
                else
                {
                    // Perform upgrade.
        
                    // If the upgrade succeeds, it will terminate the process and we don't proceed with Establish.
                    // If it fails, we DO proceed with Establish -- using the old (current) version.  One scenario
                    // in this case is if the binary is on read-only media.
                    // NOTE: means the server should always support old versions... which for now just means
                    // supporting Establish() etc. as we're already past the handshake.

                    if (manager->DoUpgrade(downloadResponse))
                    {
                        // NOTE: state will remain INITIALIZING.  The app is terminating.
                        return 0;
                    }
                    // else fall through to Establish()
                }
            }

            try
            {
                // Establish VPN connection and wait for termination
                // Throws TryNextServer or Abort on failure
                
                DoVPNConnection(manager, serverEntry);
            }
            catch (TryNextServer&)
            {
                // When the VPN attempt fails, establish SSH connection and wait for termination
                manager->RemoveVPNConnection();
                DoSSHConnection(manager);
            }

            break;
        }
        catch (Abort&)
        {
            manager->RemoveVPNConnection();
            manager->SSHDisconnect();
            manager->SetState(CONNECTION_MANAGER_STATE_STOPPED);
            break;
        }
        catch (TryNextServer&)
        {
            manager->RemoveVPNConnection();
            manager->SSHDisconnect();
            manager->MarkCurrentServerFailed();
            // Continue while loop to try next server
        }
    }

    return 0;
}

// ==== VPN Session Functions =================================================

VPNConnectionState ConnectionManager::GetVPNConnectionState(void)
{
    AutoMUTEX lock(m_mutex);
    
    return m_vpnConnection.GetState();
}

HANDLE ConnectionManager::GetVPNConnectionStateChangeEvent(void)
{
    AutoMUTEX lock(m_mutex);
    
    return m_vpnConnection.GetStateChangeEvent();
}

void ConnectionManager::RemoveVPNConnection(void)
{
    AutoMUTEX lock(m_mutex);

    m_vpnConnection.Remove();
}

void ConnectionManager::VPNEstablish(void)
{
    // Kick off the VPN connection establishment

    AutoMUTEX lock(m_mutex);
    
    if (!m_vpnConnection.Establish(NarrowToTString(m_currentSessionInfo.GetServerAddress()),
                                   NarrowToTString(m_currentSessionInfo.GetPSK())))
    {
        // This is a local error, we should not try the next server because
        // we'll likely end up in an infinite loop.
        throw Abort();
    }
}

void ConnectionManager::WaitForVPNConnectionStateToChangeFrom(VPNConnectionState state)
{
    // NOTE: no lock, as in ConnectionManagerStartThread

    while (state == GetVPNConnectionState())
    {
        HANDLE stateChangeEvent = GetVPNConnectionStateChangeEvent();

        // Wait for RasDialCallback to set a new state, or timeout (to check cancel/termination)
        DWORD result = WaitForSingleObject(stateChangeEvent, 100);

        if (GetUserSignalledStop() || result == WAIT_FAILED || result == WAIT_ABANDONED)
        {
            throw Abort();
        }
    }
}

// ==== SSH Session Functions =================================================

bool ConnectionManager::SSHConnect(
        const tstring& sshServerAddress,
        const tstring& sshServerPort,
        const tstring& sshServerPublicKey,
        const tstring& sshUsername,
        const tstring& sshPassword)
{
    AutoMUTEX lock(m_mutex);

    return m_sshConnection.Connect(
            sshServerAddress,
            sshServerPort,
            sshServerPublicKey,
            sshUsername,
            sshPassword);
}

void ConnectionManager::SSHDisconnect(void)
{
    // Note: no lock

    m_sshConnection.Disconnect();
}

bool ConnectionManager::SSHWaitForConnected(void)
{
    // Note: no lock

    return m_sshConnection.WaitForConnected();
}

void ConnectionManager::SSHWaitAndDisconnect(void)
{
    // Note: no lock

    m_sshConnection.WaitAndDisconnect();
}

void ConnectionManager::MarkCurrentServerFailed(void)
{
    AutoMUTEX lock(m_mutex);
    
    m_vpnList.MarkCurrentServerFailed();
}

// ==== General Session Functions =============================================

tstring ConnectionManager::GetConnectRequestPath(void)
{
    AutoMUTEX lock(m_mutex);

    return tstring(HTTP_CONNECTED_REQUEST_PATH) + 
           _T("?propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
           _T("&vpn_client_ip_address=") + m_vpnConnection.GetPPPIPAddress();
}

tstring ConnectionManager::GetFailedRequestPath(void)
{
    AutoMUTEX lock(m_mutex);

    std::stringstream s;
    s << m_vpnConnection.GetLastVPNErrorCode();

    return tstring(HTTP_FAILED_REQUEST_PATH) + 
           _T("?propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret()) +
           _T("&error_code=") + NarrowToTString(s.str());
}

void ConnectionManager::LoadNextServer(
        ServerEntry& serverEntry,
        tstring& handshakeRequestPath)
{
    // Select next server to try to connect to

    AutoMUTEX lock(m_mutex);
    
    try
    {
        // Try the next server in our list.
        serverEntry = m_vpnList.GetNextServer();
    }
    catch (std::exception &ex)
    {
        my_print(false, string("LoadNextServer caught exception: ") + ex.what());
        throw Abort();
    }

    // Current session holds server entry info and will also be loaded
    // with homepage and other info.

    m_currentSessionInfo.Set(serverEntry);

    // Output values used in next TryNextServer step

    handshakeRequestPath = tstring(HTTP_HANDSHAKE_REQUEST_PATH) + 
                           _T("?propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
                           _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
                           _T("&client_version=") + NarrowToTString(CLIENT_VERSION) +
                           _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret());

    // Include a list of known server IP addresses in the request query string as required by /handshake

    ServerEntries serverEntries =  m_vpnList.GetList();
    for (ServerEntryIterator ii = serverEntries.begin(); ii != serverEntries.end(); ++ii)
    {
        handshakeRequestPath += _T("&known_server=");
        handshakeRequestPath += NarrowToTString(ii->serverAddress);
    }
}

void ConnectionManager::HandleHandshakeResponse(const char* handshakeResponse)
{
    // Parse handshake response
    // - get PSK, which we use to connect to VPN
    // - get homepage, which we'll launch later
    // - add discovered servers to local list

    AutoMUTEX lock(m_mutex);
    
    if (!m_currentSessionInfo.ParseHandshakeResponse(handshakeResponse))
    {
        my_print(false, _T("HandleHandshakeResponse: ParseHandshakeResponse failed."));
        throw TryNextServer();
    }

    try
    {
        m_vpnList.AddEntriesToList(m_currentSessionInfo.GetDiscoveredServerEntries());
    }
    catch (std::exception &ex)
    {
        my_print(false, string("HandleHandshakeResponse caught exception: ") + ex.what());
        // This isn't fatal.  The VPN connection can still be established.
    }
}

bool ConnectionManager::RequireUpgrade(tstring& downloadRequestPath)
{
    AutoMUTEX lock(m_mutex);
    
    if (m_currentSessionInfo.GetUpgradeVersion().size() > 0)
    {
        downloadRequestPath = tstring(HTTP_DOWNLOAD_REQUEST_PATH) + 
                              _T("?propagation_channel_id=") + NarrowToTString(PROPAGATION_CHANNEL_ID) +
                              _T("&sponsor_id=") + NarrowToTString(SPONSOR_ID) +
                              _T("&client_version=") + NarrowToTString(m_currentSessionInfo.GetUpgradeVersion()) +
                              _T("&server_secret=") + NarrowToTString(m_currentSessionInfo.GetWebServerSecret());
        return true;
    }

    return false;
}

bool ConnectionManager::DoUpgrade(const string& download)
{
    AutoMUTEX lock(m_mutex);

    // Find current process binary path

    TCHAR filename[1000];
    if (!GetModuleFileName(NULL, filename, 1000))
    {
        // Abort upgrade: Establish() will proceed.
        return false;
    }

    // Rename current binary to archive name

    tstring archive_filename(filename);
    archive_filename += _T(".orig");

    bool bArchiveCreated = false;

    try
    {
        // We can't delete/modify the binary for a running Windows process,
        // so instead we move the running binary to an archive filename and
        // write the new version to the original filename.

        if (!DeleteFile(archive_filename.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            throw std::exception("Upgrade - DeleteFile failed");
        }

        if (!MoveFile(filename, archive_filename.c_str()))
        {
            throw std::exception("Upgrade - MoveFile failed");
        }

        bArchiveCreated = true;

        // Write new version to current binary file name

        AutoHANDLE file = CreateFile(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (file == INVALID_HANDLE_VALUE)
        {
            throw std::exception("Upgrade - CreateFile failed");
        }

        DWORD written;

        if (!WriteFile(file, download.c_str(), download.length(), &written, NULL) || written != download.length())
        {
            throw std::exception("Upgrade - WriteFile failed");
        }

        if (!FlushFileBuffers(file))
        {
            throw std::exception("Upgrade - FlushFileBuffers failed");
        }
    }
    catch (std::exception& ex)
    {
        std::stringstream s;
        s << ex.what() << " (" << GetLastError() << ")";
        my_print(false, s.str().c_str());
        
        // Try to restore the original version
        if (bArchiveCreated)
        {
            CopyFile(archive_filename.c_str(), filename, FALSE);
        }

        // Abort upgrade: Establish() will proceed.
        return false;
    }

    // Don't teardown connection: see comment in VPNConnection::Remove

    m_vpnConnection.SuspendTeardownForUpgrade();

    // Die & respawn
    // TODO: if ShellExecute fails, don't die?

    ShellExecute(0, NULL, filename, 0, 0, SW_SHOWNORMAL);
    PostMessage(g_hWnd, WM_QUIT, 0, 0);

    return true;
}
