/*
 * Copyright (c) 2012, Psiphon Inc.
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

#pragma once

#include "ras.h"
#include "tstring.h"


static int VPN_CONNECTION_TIMEOUT_SECONDS = 20;
static const TCHAR* VPN_CONNECTION_NAME = _T("Psiphon3");


class VPNTransport: public TransportBase
{
    enum ConnectionState
    {
        CONNECTION_STATE_STOPPED = 0,
        CONNECTION_STATE_STARTING,
        CONNECTION_STATE_CONNECTED,
        CONNECTION_STATE_FAILED
    };

public:
    VPNTransport(ConnectionManager* manager); 
    virtual ~VPNTransport();

    virtual tstring GetTransportName() const;
    virtual tstring GetSessionID(SessionInfo sessionInfo) const;
    virtual tstring GetLastTransportError() const;

    virtual void WaitForDisconnect();
    virtual bool Cleanup();

protected:
    virtual void TransportConnect(const SessionInfo& sessionInfo);
    
    void TransportConnectHelper(const SessionInfo& sessionInfo);
    bool ServerVPNCapable(const SessionInfo& sessionInfo) const;
    ConnectionState GetConnectionState() const;
    void SetConnectionState(ConnectionState newState);
    HANDLE GetStateChangeEvent();
    void SetLastErrorCode(unsigned int lastErrorCode);
    unsigned int GetLastErrorCode() const;
    void WaitForConnectionStateToChangeFrom(ConnectionState state);
    tstring GetPPPIPAddress() const;
    HRASCONN GetActiveRasConnection();
    bool Establish(const tstring& serverAddress, const tstring& PSK);
    static void CALLBACK RasDialCallback(
                            DWORD userData,
                            DWORD,
                            HRASCONN rasConnection,
                            UINT,
                            RASCONNSTATE rasConnState,
                            DWORD dwError,
                            DWORD);

private:
    ConnectionState m_state;
    HANDLE m_stateChangeEvent;
    HRASCONN m_rasConnection;
    unsigned int m_lastErrorCode;
};
