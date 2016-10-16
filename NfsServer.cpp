#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>

#include "Common.h"
#include "SelectServer.h"
#include "Rpc.h"

// TODO: log settings
// --------------------------------------------------------
//
// Categories
// --------------------------------------------------------
// socket (select server log)
// network (tcp connections, and tcp/udp sends/receives)
// rpc
// nfs
// portmap
//
// Levels
// --------------------------------------------------------
// error
// debug
// info
// 

#define SHARED_BUFFER_SIZE 8192

#define MAX_IP_STRING   39 // A full IPv6 string like 2001:0db8:85a3:0000:0000:8a2e:0370:7334
#define MAX_PORT_STRING  5 // 65535
#define MAX_ADDR_STRING (MAX_IP_STRING + 1 + MAX_PORT_STRING)

#define STATIC_ARRAY_LENGTH(arr) (sizeof(arr)/sizeof(arr[0]))

void AddrToString(char dest[], sockaddr* addr)
{
    if(addr->sa_family == AF_INET)
    {
        unsigned short port = htons(((sockaddr_in*)addr)->sin_port);
        u_long ipv4 = htonl(((sockaddr_in*)addr)->sin_addr.s_addr);
        sprintf(dest, "%u.%u.%u.%u:%u",
            ipv4 >> 24, (ipv4 >> 16) & 0xFF, (ipv4 >> 8) & 0xFF,
            ipv4 & 0xFF, port);
    }
    else if(addr->sa_family == AF_INET6)
    {
        unsigned short port = htons(((sockaddr_in6*)addr)->sin6_port);
        sprintf(dest, "<ipv6-addr>:%u", port);
    }
    else
    {
        sprintf(dest, "<address-family:%d>", addr->sa_family);
    }
}


// All the data from the Rpc call that the handler
// will need for the response
struct RpcCallInfo
{
    UINT xid;
    UINT rpcVersion;
    UINT program;
    UINT programVersion;
    UINT procedure;
};


#define RPC_PROG_MISMATCH_LENGTH 36
void AppendProgMismatch(char* sharedBuffer, UINT xid, UINT min, UINT max)
{
    AppendUint(sharedBuffer +  0, RPC_LAST_FRAGMENT_FLAG | (RPC_PROG_MISMATCH_LENGTH-4));
    AppendUint(sharedBuffer +  4, xid);
    AppendUint(sharedBuffer +  8, RPC_MESSAGE_TYPE_REPLY);
    AppendUint(sharedBuffer + 12, RPC_REPLY_ACCEPTED);
    AppendUint(sharedBuffer + 16, RPC_AUTH_FLAVOR_NULL);
    AppendUint(sharedBuffer + 20, 0); // Auth is length 0
    AppendUint(sharedBuffer + 24, RPC_REPLY_ACCEPT_STATUS_PROG_MISMATCH);
    AppendUint(sharedBuffer + 28, min);
    AppendUint(sharedBuffer + 32, max);
}
// Erros that have no data are PROG_UNAVAIL, PROC_UNAVAIL and GARBAGE_ARGS
#define RPC_ERROR_NO_DATA_LENGTH 28
void AppendRpcErrorNoData(char* sharedBuffer, UINT xid, UINT error)
{
    AppendUint(sharedBuffer +  0, RPC_LAST_FRAGMENT_FLAG | (RPC_ERROR_NO_DATA_LENGTH-4));
    AppendUint(sharedBuffer +  4, xid);
    AppendUint(sharedBuffer +  8, RPC_MESSAGE_TYPE_REPLY);
    AppendUint(sharedBuffer + 12, RPC_REPLY_ACCEPTED);
    AppendUint(sharedBuffer + 16, RPC_AUTH_FLAVOR_NULL);
    AppendUint(sharedBuffer + 20, 0); // Auth is length 0
    AppendUint(sharedBuffer + 24, error);
}

class RpcProgram
{
  public:
    const char* name;
    const UINT program;
    const UINT minVersion;
    const UINT maxVersion;
    RpcProgram(const char* name, const UINT program, UINT minVersion, UINT maxVersion) :
        name(name), program(program), minVersion(minVersion), maxVersion(maxVersion)
    {
    }
    virtual void HandleCall(SelectSock* sock, RpcCallInfo* callInfo, char* sharedBuffer, char* command, char* limit) = 0;
};



class PortmapProgram : public RpcProgram
{
  public:
    PortmapProgram() : RpcProgram("PortMap", RPC_PROGRAM_PORTMAP, 2, 2)
    {
    }
    // Note: it is very likely that sharedBuffer will overlap with command.  Only use
    //       the shared buffer if you are done with the command.
    void HandleCall(SelectSock* sock, RpcCallInfo* callInfo, char* sharedBuffer, char* command, char* limit)
    {
        if(PROC_NULL == callInfo->procedure)
        {
            AppendUint(sharedBuffer +  0, RPC_LAST_FRAGMENT_FLAG | 24);
            AppendUint(sharedBuffer +  4, callInfo->xid);
            AppendUint(sharedBuffer +  8, RPC_MESSAGE_TYPE_REPLY);
            AppendUint(sharedBuffer + 12, RPC_REPLY_ACCEPTED);
            AppendUint(sharedBuffer + 16, RPC_AUTH_FLAVOR_NULL);
            AppendUint(sharedBuffer + 20, 0); // Auth is length 0
            AppendUint(sharedBuffer + 24, 0); // Success
            // TODO: check send return value
            send(sock->so, (char*)sharedBuffer, 28, 0);
            LOG("[PORTMAP] NULL(s=%d)", sock->so);
        }
        else
        {
            LOG("[PORTMAP] unhandled procedure %u", callInfo->procedure);
            AppendRpcErrorNoData(sharedBuffer, callInfo->xid, RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL);
            // TODO: check send return value
            send(sock->so, (char*)sharedBuffer, RPC_ERROR_NO_DATA_LENGTH, 0);
        }
    }
};

class NfsProgram : public RpcProgram
{
    #define NFS3_RESPONSE_OK 0
    #define NFS3_RESPONSE_OK 0
    #define NFS3_PROCEDURE_NULL 0

  public:
    NfsProgram() : RpcProgram("Nfs", RPC_PROGRAM_NFS, 3, 3)
    {
    }
    // Note: it is very likely that sharedBuffer will overlap with command.  Only use
    //       the shared buffer if you are done with the command.
    void HandleCall(SelectSock* sock, RpcCallInfo* callInfo, char* sharedBuffer, char* command, char* limit)
    {
        if(PROC_NULL == callInfo->procedure)
        {
            AppendUint(sharedBuffer +  0, RPC_LAST_FRAGMENT_FLAG | 24);
            AppendUint(sharedBuffer +  4, callInfo->xid);
            AppendUint(sharedBuffer +  8, RPC_MESSAGE_TYPE_REPLY);
            AppendUint(sharedBuffer + 12, RPC_REPLY_ACCEPTED);
            AppendUint(sharedBuffer + 16, RPC_AUTH_FLAVOR_NULL);
            AppendUint(sharedBuffer + 20, 0); // Auth is length 0
            AppendUint(sharedBuffer + 24, 0); // Success
            // TODO: check send return value
            send(sock->so, (char*)sharedBuffer, 28, 0);
            LOG("[NFS] NULL(s=%d)", sock->so);
        }
        else
        {
            LOG("[NFS] unhandled procedure %u", callInfo->procedure);
            AppendRpcErrorNoData(sharedBuffer, callInfo->xid, RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL);
            // TODO: check send return value
            send(sock->so, (char*)sharedBuffer, RPC_ERROR_NO_DATA_LENGTH, 0);
        }
    }
};

PortmapProgram portmapProgram;
NfsProgram nfsProgram;

RpcProgram* Programs[] = {
    &portmapProgram,
    &nfsProgram,
};




// Return: 1 on error
// Note: it is very likely that sharedBuffer will overlap with command.  Only use
//       the shared buffer if you are done with the command.
int HandleRpcCommand(SelectSock* sock, char* sharedBuffer, char* command, char* limit)
{
    if(command + 8 > limit)
    {
        LOG_ERROR("Invalid RPC command, header not long enough");
        return 1; // error
    }

    RpcCallInfo callInfo;
    callInfo.xid = ParseUint(command +  0);
    UINT messageType  = ParseUint(command +  4);
    if(messageType == RPC_MESSAGE_TYPE_CALL)
    {
        command += 8;
        if(command + 28 > limit)
        {
            LOG("[RPC] Invalid RPC command, header not long enough");
            return 1; // error
        }
        callInfo.rpcVersion     = ParseUint(command +  0);
        if(callInfo.rpcVersion != 2)
        {
            LOG("[RPC] unsupported rpc version %u", callInfo.rpcVersion);
            return 1; // error
        }
        // TODO: check rpc version and send error if not matched
        
        callInfo.program        = ParseUint(command +  4);
        callInfo.programVersion = ParseUint(command +  8);
        callInfo.procedure      = ParseUint(command + 12);

        UINT credentialsAuthFlavor = ParseUint(command + 16);
        UINT credentialsLength     = ParseUint(command + 20);
        if(credentialsLength > 400)
        {
            LOG("[RPC] Invalid RPC command, credentials length %u is too long", credentialsLength);
            return 1; // error
        }
        command += 24 + credentialsLength;
        if(command + 8 > limit)
        {
            LOG("[RPC] Invalid RPC command, header not long enough");
            return 1; // error
        }
        UINT verifierAuthFlavor = ParseUint(command + 0);
        UINT verifierLength     = ParseUint(command + 4);
        if(verifierLength > 400)
        {
            LOG("[RPC] Invalid RPC command, verifier length %u is too long", verifierLength);
            return 1; // error
        }
        command += 8 + verifierLength;
        if(command > limit)
        {
            LOG("[RPC] Invalid RPC command, header not long enough");
            return 1; // error
        }

        LOG("[RPC] HandleRpcCommand(s=%d) xid 0x%08x, type %u, rpcv %u, prog %u, progv %u, proc %u, cred %u, verf %u, data_length %u",
            sock->so, callInfo.xid, messageType, callInfo.rpcVersion, callInfo.program, callInfo.programVersion, callInfo.procedure,
            credentialsAuthFlavor, verifierAuthFlavor, limit - command);

        bool foundProgram = false;
        for(unsigned i = 0; i < STATIC_ARRAY_LENGTH(Programs); i++)
        {
            if(Programs[i]->program == callInfo.program)
            {
                foundProgram = true;
                
                if(callInfo.programVersion < Programs[i]->minVersion ||
                   callInfo.programVersion > Programs[i]->minVersion)
                {
                    LOG("[RPC] Program %s(%u) does not support version %u", Programs[i]->name, callInfo.program, callInfo.programVersion);
                    AppendProgMismatch(sharedBuffer, callInfo.xid, Programs[i]->minVersion, Programs[i]->minVersion);
                    send(sock->so, (char*)sharedBuffer, RPC_PROG_MISMATCH_LENGTH, 0);
                }
                else
                {
                    Programs[i]->HandleCall(sock, &callInfo, sharedBuffer, command, limit);
                    break;
                }
            }
        }

        if(!foundProgram)
        {
            LOG("[RPC] program %u unavailable", callInfo.program);
            AppendUint(sharedBuffer +  0, RPC_LAST_FRAGMENT_FLAG | 24);
            AppendUint(sharedBuffer +  4, callInfo.xid);
            AppendUint(sharedBuffer +  8, RPC_MESSAGE_TYPE_REPLY);
            AppendUint(sharedBuffer + 12, RPC_REPLY_ACCEPTED);
            AppendUint(sharedBuffer + 16, RPC_AUTH_FLAVOR_NULL);
            AppendUint(sharedBuffer + 20, 0); // Auth is length 0
            AppendUint(sharedBuffer + 24, RPC_REPLY_ACCEPT_STATUS_PROG_UNAVAIL);
            send(sock->so, (char*)sharedBuffer, 28, 0);
        }
        
        return 0;
    }
    else
    {
        LOG_ERROR("Unhandled rpc message type %d", messageType);
        return 1; // error
    }
}


void RpcTcpRecvHandler(SynchronizedSelectServer server, SelectSock* sock, PopReason reason, char* sharedBuffer)
{
    // TODO: check if sock has an existing buffer, use that instead
    int size = recv(sock->so, (char*)sharedBuffer, SHARED_BUFFER_SIZE, 0);
    if(size <= 0)
    {
        if(size == 0)
        {
            LOG("[NET] RpcTcpRecvHandler(s=%d) client closed", sock->so);
        }
        else
        {
            LOG("[NET] RpcTcpRecvHandler(s=%d) recv returned error (return=%d, e=%d)", sock->so, size, GetLastError());
        }
        goto ERROR_EXIT;
    }
    LOG("[NET] RpcTcpRecvHandler(s=%d) Got %u bytes", sock->so, size);
    char* data = sharedBuffer;

    bool lastFragment = data[0] >> 7;
    UINT fragmentLength =
        (data[0] & 0x7F) << 24 |
        data[1] << 16 |
        data[2] <<  8 |
        data[3]       ;
    if(!lastFragment)
    {
        LOG_ERROR("multiple fragments not implemented");
        goto ERROR_EXIT;
    }
    LOG("[RPC] RpcTcpRecvHandler(s=%d) fragment length is %u", sock->so, fragmentLength);
    if(4 + fragmentLength != size)
    {
        LOG_ERROR("multiple recvs per fragment not implemented (received %u, 4+fragmentLength %u)",
            size, 4+fragmentLength);
        goto ERROR_EXIT;
    }

    if(HandleRpcCommand(sock, sharedBuffer, sharedBuffer + 4, sharedBuffer + size))
    {
        goto ERROR_EXIT;
    }
    return;

  ERROR_EXIT:
    LOG("[NET] RpcTcpRecvHandler(s=%u) closing", sock->so);
    if(sock->user)
    {
        delete sock->user;
        sock->user = NULL;
    }
    shutdown(sock->so, SD_BOTH);
    closesocket(sock->so);
    sock->UpdateEventFlags(SelectSock::NONE);
}
void TcpAcceptHandler(SynchronizedSelectServer server, SelectSock* sock, PopReason reason, char* sharedBuffer)
{
    sockaddr_in addr;
    int addrSize = sizeof(addr);
    SOCKET newSock = accept(sock->so, (sockaddr*)&addr, &addrSize);
    if(INVALID_SOCKET == newSock)
    {
        LOG("[NET] TcpAcceptHandler(s=%d) accept failed (e=%d)", sock->so, GetLastError());
    }
    char addrString[MAX_ADDR_STRING+1];
    AddrToString(addrString, (sockaddr*)&addr);

    if(server.TryAddSock(SelectSock(newSock, NULL, &RpcTcpRecvHandler, SelectSock::READ, SelectSock::INF)))
    {
        LOG("[NET] TcpAcceptHandler(s=%d) server full, rejected socket (s=%d) from '%s'", sock->so, newSock, addrString);
        shutdown(newSock, SD_BOTH);
        closesocket(newSock);
        return;
    }
    LOG("[NET] TcpAcceptHandler(s=%d) accepted new connection (s=%d) from '%s'", sock->so, newSock, addrString);
}

bool nfs2Enabled = false;
bool nfs3Enabled = true;

#define PORTMAP_PORT 111
#define NFS_PORT     2049
#define LISTEN_BACKLOG 8


// Design Note:
// This server will support any rpc program on any of the ports.
// It uses the RPC program number to determine which program is actually being called.


int RunNfsServer()
{
    SelectServer server;
    {
        // TODO: I don't like that I have to lock the server
        //       because it hasn't started yet.
        LockedSelectServer locked(&server);
        {
            sockaddr_in addr;
            addr.sin_family = AF_INET;

            // If NFSv2 or NVSv3 is enabled, then most clients
            // will expect the PORTMAP service to be listening on port 111
            if(nfs2Enabled || nfs3Enabled)
            {
                SOCKET so = socket(addr.sin_family, SOCK_STREAM, IPPROTO_TCP);
                if(so == INVALID_SOCKET)
                {
                    LOG_ERROR("socket function failed (e=%d)", GetLastError());
                    return 1; // error
                }
                addr.sin_port = htons(PORTMAP_PORT);
                addr.sin_addr.s_addr = 0;
                LOG("(s=%d) Adding TCP Listener on port %u", so, PORTMAP_PORT);
                if(SOCKET_ERROR == bind(so, (sockaddr*)&addr, sizeof(addr)))
                {
                    LOG_ERROR("bind failed (e=%d)", GetLastError());
                    return 1; // error
                }
                if(SOCKET_ERROR == listen(so, LISTEN_BACKLOG))
                {
                    LOG_ERROR("listen failed (e=%d)", GetLastError());
                    return 1; // error
                }
                locked.TryAddSock(SelectSock(so, NULL, &TcpAcceptHandler, SelectSock::READ, SelectSock::INF));
            }

            {
                SOCKET so = socket(addr.sin_family, SOCK_STREAM, IPPROTO_TCP);
                if(so == INVALID_SOCKET)
                {
                    LOG_ERROR("socket function failed (e=%d)", GetLastError());
                    return 1; // error
                }
                addr.sin_port = htons(NFS_PORT);
                addr.sin_addr.s_addr = 0;
                LOG("(s=%d) Adding TCP Listener on port %u", so, NFS_PORT);
                if(SOCKET_ERROR == bind(so, (sockaddr*)&addr, sizeof(addr)))
                {
                    LOG_ERROR("bind failed (e=%d)", GetLastError());
                    return 1; // error
                }
                if(SOCKET_ERROR == listen(so, LISTEN_BACKLOG))
                {
                    LOG_ERROR("listen failed (e=%d)", GetLastError());
                    return 1; // error
                }
                locked.TryAddSock(SelectSock(so, NULL, &TcpAcceptHandler, SelectSock::READ, SelectSock::INF));
            }
        }
    }
    char* buffer = (char*)malloc(SHARED_BUFFER_SIZE);
    if(!buffer) {
        LOG_ERROR("malloc(%d) failed", SHARED_BUFFER_SIZE);
    }
    LOG("Starting Server...");
    return server.Run(buffer, SHARED_BUFFER_SIZE);
}

