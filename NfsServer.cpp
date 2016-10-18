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

#define LITERAL_LENGTH(str) (sizeof(str)-1)
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

// Length is 20 bytes
void SetupReply(char* buffer, UINT xid)
{
    AppendUint(buffer +  0, xid);
    SET_UINT  (buffer +  4, RPC_MESSAGE_TYPE_REPLY_NETWORK_ORDER);
    SET_UINT  (buffer +  8, RPC_REPLY_ACCEPTED_NETWORK_ORDER);
    SET_UINT  (buffer + 12, RPC_AUTH_FLAVOR_NULL_NETWORK_ORDER);
    SET_UINT  (buffer + 16, 0); // Auth is length 0
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

    // Some RPC functions will handle sending the reply themselves, some
    // can return a length that indicates the reply is in the buffer and should
    // be sent by the caller
    virtual UINT HandleCall(SelectSock* sock, RpcCallInfo* callInfo, char* sharedBuffer, char* command, char* limit) = 0;
};

int sendWithLog(const char* context, SOCKET so, char* buffer, UINT length)
{
    int sent = send(so, buffer, length, 0);
    if(sent != length)
    {
        if(sent < 0)
        {
            LOG_ERROR("%s(s=%u) send %u bytes failed (returned %d, e=%d)", context, so, length, sent, GetLastError());
        }
        else
        {
            LOG_ERROR("%s(s=%u) send %u bytes returned %d (e=%d)", context, so, length, sent, GetLastError());
        }
    }
    return sent;
}

// The offset of an rpc reply for the handle call
#define REPLY_OFFSET 24

class PortmapProgram : public RpcProgram
{
  public:
    PortmapProgram() : RpcProgram("PortMap", RPC_PROGRAM_PORTMAP, 2, 2)
    {
    }
    // Note: it is very likely that sharedBuffer will overlap with command.  Only use
    //       the shared buffer if you are done with the command.
    UINT HandleCall(SelectSock* sock, RpcCallInfo* callInfo, char* sharedBuffer, char* command, char* limit)
    {
        switch(callInfo->procedure)
        {
          case PROC_NULL: // 0
            LOG("[PORTMAP] NULL(s=%u)", sock->so);
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            return 4;
          case PORTMAP_PROC_GETPORT: // 3
          {
            // for now, I'm just going to return port 111 for every
            // GETPORT request.  One technique would be to just return
            // whichever port the current socket has connected to, so that
            // way, you are basically guaranteed that it can connect to this
            // port again.  In most cases, that's going to be port 111 so that's
            // what I'll use for now.
            LOG("[PORTMAP] GETPORT(s=%u) > %u", sock->so, 111);
            SET_UINT  (sharedBuffer + REPLY_OFFSET + 0, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            AppendUint(sharedBuffer + REPLY_OFFSET + 4, 111); // port 111
            return 8;
          }
          default:
            LOG("[PORTMAP] unhandled procedure %u", callInfo->procedure);
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL_NETWORK_ORDER);
            return 4;
        }
    }
};

UINT Align4(UINT x)
{
    UINT mod = x&3;
    return mod ? x+4-mod : x;
}

//
// This implementation is temporary
//
struct NameHandle
{
    String localName;
};
static NameHandle nameHandles[100]; // TODO: size will not be hardcoded in final solution
static UINT nameHandleCount = 0;
UINT GetOrCreateHandle(String localName)
{
    for(UINT i = 0; i < nameHandleCount; i++)
    {
        if(localName.Equals(nameHandles[i].localName))
        {
            return i; // index is the handle for now
        }
    }
    UINT handle = nameHandleCount++;
    char* buffer = (char*)malloc(localName.length+1); // add 1 for '\0'
    memcpy(buffer, localName.ptr, localName.length);
    buffer[localName.length] = '\0';
    //nameHandles[handle].localName = String(buffer, localName.length);
    nameHandles[handle].localName = String(buffer, localName.length);
    LOG("Added path(handle=%u, length=%u, value='%s')",
        handle, localName.length, nameHandles[handle].localName.ptr);
    return handle;
}
struct Export
{
    String exportName;
    String localName;
};

// TODO: make this configuration loaded at runtim
Export exports[] = {
    {String("/share", LITERAL_LENGTH("/share")),
     String("C:\\", LITERAL_LENGTH("C:\\"))},
};

// Returns: response length
UINT MNT(String pathString, char* buffer)
{
    // Find the export
    UINT match = 0xFFFFFFFF;
    for(UINT i = 0; i < STATIC_ARRAY_LENGTH(exports); i++)
    {
        if(pathString.Equals(exports[i].exportName))
        {
            match = i;
            break;
        }
    }
    if(match == 0xFFFFFFFF)
    {
        LOG("[MOUNT] MNT: path '%.*s' is not exported", pathString.length, pathString.ptr);
        SET_UINT(buffer, MOUNT3_ERROR_NOENT_NETWORK_ORDER);
        return 4;
    }
    LOG("[MOUNT] path(length=%u, value=\"%.*s\") matched '%s'",
        pathString.length, pathString.length, pathString.ptr, exports[match].localName.ptr);

    // TODO: check if it is a valid mount point
    UINT handle = GetOrCreateHandle(exports[match].localName);

    // Send reply
    SET_UINT         (buffer +  0, MOUNT3_STATUS_OK_NETWORK_ORDER);
    SET_UINT         (buffer +  4, _4_NETWORK_ORDER);
    AppendUint       (buffer +  8, handle);
    SET_UINT         (buffer + 12, RPC_AUTH_FLAVOR_NULL_NETWORK_ORDER);
    return 16;
}

class MountProgram : public RpcProgram
{
    // Constants taken from RFC1813
    #define MOUNT3_MAX_PATH        1024
    #define MOUNT3_MAX_NAME        255
    #define MOUNT3_MAX_FILE_HANDLE 64

  public:
    MountProgram() : RpcProgram("Mount", RPC_PROGRAM_MOUNT, 3, 3)
    {
    }
    // Note: it is very likely that sharedBuffer will overlap with command.  Only use
    //       the shared buffer if you are done with the command.
    UINT HandleCall(SelectSock* sock, RpcCallInfo* callInfo, char* sharedBuffer, char* command, char* limit)
    {
        switch(callInfo->procedure)
        {
          case PROC_NULL: // 0
            LOG("[MOUNT] NULL(s=%u)", sock->so);
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            return 4;
          case MOUNT3_PROC_MNT: // 1
          {
            String pathString;
            pathString.length = ParseUint(command);
            pathString.ptr = command + 4;
            if(pathString.ptr + Align4(pathString.length) != limit)
            {
                LOG_ERROR("[MOUNT] MNT has a bad path length %u (actualSize is %u)", pathString.length, limit-pathString.ptr);
                // TODO: setup error
                return 0;
            }
            UINT length = MNT(pathString, sharedBuffer + REPLY_OFFSET + 4);
            AppendUint(sharedBuffer, RPC_LAST_FRAGMENT_FLAG | (24+length));
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            return length + 4;
          }
          default:
            LOG("[MOUNT] unhandled procedure %u", callInfo->procedure);
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL_NETWORK_ORDER);
            return 4;
        }
    }
};

String TryLookupHandle(char* handleBuffer, UINT handleLength)
{
    if(handleLength != 4)
    {
        LOG_ERROR("handle length of %u is not supported", handleLength);
        return String(); // indicate error
    }
    UINT handle = ParseUint(handleBuffer);
    if(handle >= nameHandleCount)
    {
        LOG_ERROR("handle %u is out of range", handle);
        return String(); // indicate error
    }
    return nameHandles[handle].localName;
}

// MODE BITS
#define MODE_OWNER_READ   0x0100
#define MODE_OWNER_WRITE  0x0080
#define MODE_OWNER_EXEC   0x0040
#define MODE_GROUP_READ   0x0020
#define MODE_GROUP_WRITE  0x0010
#define MODE_GROUP_EXEC   0x0008
#define MODE_OTHER_READ   0x0004
#define MODE_OTHER_WRITE  0x0002
#define MODE_OTHER_EXEC   0x0001

// Returns: response length
UINT GETATTR(char* handle, UINT handleLength, char* buffer)
{
    // lookup handle
    String localName = TryLookupHandle(handle, handleLength);
    if(localName.ptr == NULL)
    {
        LOG("[NFS] GETATTR: bad handle");
        SET_UINT(buffer    , NFS3_ERROR_BADHANDLE_NETWORK_ORDER);
        SET_UINT(buffer + 4, 0); // no post_op_attr
        return 8;
    }

    DWORD attributes = GetFileAttributes(handle);
    LOG("[NFS] GETATTR: attributes = 0x%08x", attributes);

    //SET_UINT  (buffer +  0, NFS3_STATUS_OK_NETWORK_ORDER);
    // Add attributes
    //SET_UINT  (buffer +  4,

    return 32;
    {
        LOG("[NFS] GETATTR: not implemented");
        SET_UINT(buffer    , NFS3_ERROR_BADHANDLE_NETWORK_ORDER);
        SET_UINT(buffer + 4, 0); // no post_op_attr
        return 8;
    }
}

// Returns: response length
UINT FSINFO(char* handle, UINT handleLength, char* buffer)
{
    // lookup handle
    String localName = TryLookupHandle(handle, handleLength);
    if(localName.ptr == NULL)
    {
        LOG("[NFS] FSINFO: bad handle");
        SET_UINT(buffer    , NFS3_ERROR_BADHANDLE_NETWORK_ORDER);
        SET_UINT(buffer + 4, 0); // no post_op_attr
        return 8;
    }

    SET_UINT  (buffer +  0, NFS3_STATUS_OK_NETWORK_ORDER);
    SET_UINT  (buffer +  4, 0);          // no post-op_attr
    SET_UINT  (buffer +  8, 0xFFFFFFFF); // rtmax, no limit
    SET_UINT  (buffer + 12, 0xFFFFFFFF); // rtpref, no limit
    SET_UINT  (buffer + 16, 0);          // rtmult, no preference
    SET_UINT  (buffer + 20, 0xFFFFFFFF); // wtmax, no limit
    SET_UINT  (buffer + 24, 0xFFFFFFFF); // wtpref, no limit
    SET_UINT  (buffer + 28, 0);          // wtmult, no preference
    SET_UINT  (buffer + 32, 0xFFFFFFFF); // dtpref, preferred size of a READDIR request
    SET_UINT  (buffer + 36, 0xFFFFFFFF); // maxfilesize HIGH_DWORD (no limit)
    SET_UINT  (buffer + 40, 0xFFFFFFFF); // maxfilesize LOW_DWORD  (no limit)
    SET_UINT  (buffer + 44, 0);          // time_delta (seconds)
    AppendUint(buffer + 48, 1000000);    // time_delta (nanoseconds) set to 1 millsecond for now
    SET_UINT  (buffer + 52, 0);          // properties (all off for now)
                                         // TODO: should probably set value for symbolic/hard links
    return 56;
}
// Returns: response length
UINT PATHCONF(char* handle, UINT handleLength, char* buffer)
{
    // lookup handle
    String localName = TryLookupHandle(handle, handleLength);
    if(localName.ptr == NULL)
    {
        LOG("[NFS] PATHCONF: bad handle");
        SET_UINT(buffer    , NFS3_ERROR_BADHANDLE_NETWORK_ORDER);
        SET_UINT(buffer + 4, 0); // no post_op_attr
        return 8;
    }

    SET_UINT  (buffer +  0, NFS3_STATUS_OK_NETWORK_ORDER);
    SET_UINT  (buffer +  4, 0);                // no post-op_attr
    SET_UINT  (buffer +  8, 0xFFFFFFFF);       // linkmax, no limit
    AppendUint(buffer + 12, MAX_PATH);         // name_max, no limit
    SET_UINT  (buffer + 16, _1_NETWORK_ORDER); // no-trunc (TRUE, server does not truncate names that are too large)
    SET_UINT  (buffer + 20, 0);                // chown_restricted (FALSE)
    SET_UINT  (buffer + 24, _1_NETWORK_ORDER); // case_insensitive (TRUE)
    SET_UINT  (buffer + 28, 0);                // case_preserving (FALSE)
    return 32;
}


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
    UINT HandleCall(SelectSock* sock, RpcCallInfo* callInfo, char* sharedBuffer, char* command, char* limit)
    {
        switch(callInfo->procedure)
        {
          case PROC_NULL: // 0
            LOG("[NFS] NULL(s=%u)", sock->so);
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            return 4;
          case NFS3_PROC_GETATTR: // 1
          {
            UINT handleLength = ParseUint(command);
            char* handle = command + 4;
            if(handle + Align4(handleLength) != limit)
            {
                LOG_ERROR("[NFS] GETATTR has a bad handle length %u (actualSize is %u)", handleLength, limit-handle);
                // TODO: setup error
                return 0;
            }
            LOG("[NFS] GETATTR handle is %u bytes", handleLength);
            UINT length = GETATTR(handle, handleLength, sharedBuffer + REPLY_OFFSET + 4);
            AppendUint(sharedBuffer, RPC_LAST_FRAGMENT_FLAG | (24+length));
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            return length + 4;
          }
          case NFS3_PROC_FSINFO: // 19
          {
            UINT handleLength = ParseUint(command);
            char* handle = command + 4;
            if(handle + Align4(handleLength) != limit)
            {
                LOG_ERROR("[NFS] FSINFO has a bad handle length %u (actualSize is %u)", handleLength, limit-handle);
                // TODO: setup error
                return 0;
            }
            LOG("[NFS] FSINFO handle is %u bytes", handleLength);
            UINT length = FSINFO(handle, handleLength, sharedBuffer + REPLY_OFFSET + 4);
            AppendUint(sharedBuffer, RPC_LAST_FRAGMENT_FLAG | (24+length));
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            return length + 4;
          }
          case NFS3_PROC_PATHCONF: // 20
          {
            UINT handleLength = ParseUint(command);
            char* handle = command + 4;
            if(handle + Align4(handleLength) != limit)
            {
                LOG_ERROR("[NFS] PATHCONF has a bad handle length %u (actualSize is %u)", handleLength, limit-handle);
                // TODO: setup error
                return 0;
            }
            LOG("[NFS] PATHCONF handle is %u bytes", handleLength);
            UINT length = PATHCONF(handle, handleLength, sharedBuffer + REPLY_OFFSET + 4);
            AppendUint(sharedBuffer, RPC_LAST_FRAGMENT_FLAG | (24+length));
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_SUCCESS_NETWORK_ORDER);
            return length + 4;
          }
          default:
            LOG("[NFS] unhandled procedure %u", callInfo->procedure);
            SET_UINT(sharedBuffer + REPLY_OFFSET, RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL_NETWORK_ORDER);
            return 4;
        }
    }
};

PortmapProgram portmapProgram;
MountProgram mountProgram;
NfsProgram nfsProgram;

RpcProgram* Programs[] = {
    &nfsProgram,
    &mountProgram,
    &portmapProgram,
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
            LOG_RPC("Invalid RPC command, header not long enough");
            return 1; // error
        }
        callInfo.rpcVersion     = ParseUint(command +  0);
        if(callInfo.rpcVersion != 2)
        {
            LOG_RPC("unsupported rpc version %u", callInfo.rpcVersion);
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
            LOG_RPC("Invalid RPC command, credentials length %u is too long", credentialsLength);
            return 1; // error
        }
        command += 24 + credentialsLength;
        if(command + 8 > limit)
        {
            LOG_RPC("Invalid RPC command, header not long enough");
            return 1; // error
        }
        UINT verifierAuthFlavor = ParseUint(command + 0);
        UINT verifierLength     = ParseUint(command + 4);
        if(verifierLength > 400)
        {
            LOG_RPC("Invalid RPC command, verifier length %u is too long", verifierLength);
            return 1; // error
        }
        command += 8 + verifierLength;
        if(command > limit)
        {
            LOG_RPC("Invalid RPC command, header not long enough");
            return 1; // error
        }

        LOG_RPC("HandleRpcCommand(s=%d) xid 0x%08x, type %u, rpcv %u, prog %u, progv %u, proc %u, cred %u, verf %u, data_length %u",
            sock->so, callInfo.xid, messageType, callInfo.rpcVersion, callInfo.program, callInfo.programVersion, callInfo.procedure,
            credentialsAuthFlavor, verifierAuthFlavor, limit - command);

        bool foundProgram = false;
        UINT replySize;
        for(unsigned i = 0; i < STATIC_ARRAY_LENGTH(Programs); i++)
        {
            if(Programs[i]->program == callInfo.program)
            {
                foundProgram = true;

                if(callInfo.programVersion < Programs[i]->minVersion ||
                   callInfo.programVersion > Programs[i]->minVersion)
                {
                    LOG_RPC("Program %s(%u) does not support version %u", Programs[i]->name, callInfo.program, callInfo.programVersion);
                    SET_UINT  (sharedBuffer + 24, RPC_REPLY_ACCEPT_STATUS_PROG_MISMATCH_NETWORK_ORDER);
                    AppendUint(sharedBuffer + 28, Programs[i]->minVersion);
                    AppendUint(sharedBuffer + 32, Programs[i]->maxVersion);
                    replySize = 12;
                }
                else
                {
                    replySize = Programs[i]->HandleCall(sock, &callInfo, sharedBuffer, command, limit);
                }
                break;
            }
        }

        if(!foundProgram)
        {
            LOG_RPC("program %u unavailable", callInfo.program);
            SET_UINT(sharedBuffer + 24, RPC_REPLY_ACCEPT_STATUS_PROG_UNAVAIL_NETWORK_ORDER);
            replySize = 4;
        }

        if(replySize)
        {
            AppendUint(sharedBuffer +  0, RPC_LAST_FRAGMENT_FLAG | (REPLY_OFFSET - 4 + replySize));
            SetupReply(sharedBuffer +  4, callInfo.xid);
            LOG("[DEBUG] sending %u bytes...", REPLY_OFFSET + replySize);
            sendWithLog("[RPC]", sock->so, (char*)sharedBuffer, REPLY_OFFSET + replySize);
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
            LOG_NET("RpcTcpRecvHandler(s=%d) client closed", sock->so);
        }
        else
        {
            LOG_NET("RpcTcpRecvHandler(s=%d) recv returned error (return=%d, e=%d)", sock->so, size, GetLastError());
        }
        goto ERROR_EXIT;
    }
    LOG_NET("RpcTcpRecvHandler(s=%d) Got %u bytes", sock->so, size);
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
    LOG_RPC("RpcTcpRecvHandler(s=%d) fragment length is %u", sock->so, fragmentLength);
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
    LOG_NET("RpcTcpRecvHandler(s=%u) closing", sock->so);
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
        LOG_NET("TcpAcceptHandler(s=%d) accept failed (e=%d)", sock->so, GetLastError());
    }
    char addrString[MAX_ADDR_STRING+1];
    AddrToString(addrString, (sockaddr*)&addr);

    if(server.TryAddSock(SelectSock(newSock, NULL, &RpcTcpRecvHandler, SelectSock::READ, SelectSock::INF)))
    {
        LOG_NET("TcpAcceptHandler(s=%d) server full, rejected socket (s=%d) from '%s'", sock->so, newSock, addrString);
        shutdown(newSock, SD_BOTH);
        closesocket(newSock);
        return;
    }
    LOG_NET("TcpAcceptHandler(s=%d) accepted new connection (s=%d) from '%s'", sock->so, newSock, addrString);
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

