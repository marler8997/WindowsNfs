#include <winsock2.h>
#include <stdio.h>
#include <stdarg.h>

#include "Common.h"
#include "Rpc.h"

char buffer[4096];

#define TEST_FAIL    0
#define TEST_SUCCESS 1

#define TEST_ASSERT(condition, line, fmt, ...)                              \
    do                                                                      \
    {                                                                       \
        if(!(condition))                                                    \
        {                                                                   \
            printf("TEST_ASSERT line %u: " fmt "\r\n", line,##__VA_ARGS__); \
            return TEST_FAIL;                                               \
        }                                                                   \
    } while(0);

class Connection
{
  private:
    SOCKET so;
  public:
    Connection(unsigned short port)
    {
        sockaddr_in addr;
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(0x7F000001);

        so = socket(addr.sin_family, SOCK_STREAM, IPPROTO_TCP);
        if(so == INVALID_SOCKET)
        {
            printf("Error: socket function failed (e=%d)\r\n", GetLastError());
        }
        else
        {
            if(SOCKET_ERROR == connect(so, (sockaddr*)&addr, sizeof(addr)))
            {
                printf("Error: connect function failed (e=%d)\r\n", GetLastError());
                closesocket(so);
                so = INVALID_SOCKET;
            }
        }
    }
    ~Connection()
    {
        if(so != INVALID_SOCKET)
        {
            shutdown(so, SD_BOTH);
            closesocket(so);
            so = INVALID_SOCKET;
        }
    }
    SOCKET sock()
    {
        return so;
    }
};

UINT SetupCall(UINT programNetworkOrder, UINT progVersionNetworkOrder, UINT procNetworkOrder, UINT argCount, ...)
{
    AppendUint(buffer +  0, RPC_LAST_FRAGMENT_FLAG | (40 + (argCount*4)));
    SET_UINT(buffer +  8, RPC_MESSAGE_TYPE_CALL_NETWORK_ORDER);
    SET_UINT(buffer + 12, RPC_VERSION_NETWORK_ORDER);
    SET_UINT(buffer + 16, programNetworkOrder);
    SET_UINT(buffer + 20, progVersionNetworkOrder);
    SET_UINT(buffer + 24, procNetworkOrder);
    SET_UINT(buffer + 28, RPC_AUTH_FLAVOR_NULL_NETWORK_ORDER);
    SET_UINT(buffer + 32, 0);
    SET_UINT(buffer + 36, RPC_AUTH_FLAVOR_NULL_NETWORK_ORDER);
    SET_UINT(buffer + 40, 0);
    va_list args;
    va_start(args, argCount);
    UINT offset = 44;
    for(int i = 0; i < argCount; i++)
    {
        AppendUint(buffer + offset, va_arg(args, UINT));
        offset += 4;
    }
    va_end(args);
    return offset;
}

// Tests that cannot be recovered and should be closed by the server
int TestInvalidCall(UINT xid, UINT callSize)
{
    Connection conn(2049);

    AppendUint(buffer +  4, xid);

    int sent = send(conn.sock(), buffer, callSize, 0);
    TEST_ASSERT(callSize == sent, __LINE__, "send failed, returned %d", sent);
    int received = recv(conn.sock(), buffer, sizeof(buffer), 0);
    TEST_ASSERT(received == 0, __LINE__, "expected recv to return 0 but got %d", received);
    return TEST_SUCCESS;
}

int TestCall(Connection* conn, UINT xid, UINT callSize, UINT argCount, ...)
{
    AppendUint(buffer +  4, xid);
    int sent = send(conn->sock(), buffer, callSize, 0);
    TEST_ASSERT(callSize == sent, __LINE__, "expected to send %d but sent %d", callSize, sent);
    int received = recv(conn->sock(), buffer, sizeof(buffer), 0);
    unsigned expectedReceived = 24+(4*argCount);
    TEST_ASSERT(received == expectedReceived, __LINE__, "expected to receive %d but got %d", expectedReceived, received);
    TEST_ASSERT(ParseUint(buffer) == (RPC_LAST_FRAGMENT_FLAG | (expectedReceived-4)), __LINE__,
        "expected fragment 0x%08x but got 0x%08x", (RPC_LAST_FRAGMENT_FLAG | expectedReceived), ParseUint(buffer));
    TEST_ASSERT(ParseUint(buffer +  4) == xid, __LINE__, "bad xid (expected 0x%08x, got 0x%08x)", xid, ParseUint(buffer+4));
    TEST_ASSERT(GET_UINT (buffer +  8) == RPC_MESSAGE_TYPE_REPLY_NETWORK_ORDER, __LINE__, "bad message type");
    TEST_ASSERT(GET_UINT (buffer + 12) == RPC_REPLY_ACCEPTED_NETWORK_ORDER, __LINE__, "bad reply status");
    TEST_ASSERT(GET_UINT (buffer + 16) == RPC_AUTH_FLAVOR_NULL_NETWORK_ORDER, __LINE__, "bad auth flavor");
    TEST_ASSERT(GET_UINT (buffer + 20) == 0, __LINE__, "bad auth length");
    va_list args;
    va_start(args, argCount);
    UINT offset = 24;
    for(int i = 0; i < argCount; i++)
    {
        UINT expected = va_arg(args, UINT);
        UINT actual = ParseUint(buffer + offset + (4*i));
        TEST_ASSERT(actual == expected, __LINE__, "expected replyUintData[%u] to be %u, but is %u",
            i, expected, actual);
    }
    va_end(args);
    return TEST_SUCCESS;
}

int run()
{
    Connection conn(2049);
    //
    // Test that the NULL procedures work
    //
    {
        UINT callSize = SetupCall(RPC_PROGRAM_PORTMAP_NETWORK_ORDER, _2_NETWORK_ORDER, PROC_NULL, 0);
        TEST_ASSERT(TestCall(&conn, 0x40f18a0f, callSize, 1, RPC_REPLY_ACCEPT_STATUS_SUCCESS), __LINE__, "PORTMAPv2 NULL failed");
    }
    {
        UINT callSize = SetupCall(RPC_PROGRAM_MOUNT_NETWORK_ORDER, _3_NETWORK_ORDER, PROC_NULL, 0);
        TEST_ASSERT(TestCall(&conn, 0xaf29bec8, callSize, 1, RPC_REPLY_ACCEPT_STATUS_SUCCESS), __LINE__, "MOUNTv4 NULL failed");
    }
    {
        UINT callSize = SetupCall(RPC_PROGRAM_NFS_NETWORK_ORDER, _3_NETWORK_ORDER, PROC_NULL, 0);
        TEST_ASSERT(TestCall(&conn, 0xab9828de, callSize, 1, RPC_REPLY_ACCEPT_STATUS_SUCCESS), __LINE__, "NFSv3 NULL failed");
    }

    // TODO: test malformed calls
    //   - fragment length is too short
    //   - invalid credentials and verifier lengths (too long/too short)
    //   - unknown message type (not call or reply)

    #define RPC_VERSION_OFFSET 12
    // Test invalid rpc version 0
    {
        UINT callSize = SetupCall(RPC_PROGRAM_PORTMAP_NETWORK_ORDER, _2_NETWORK_ORDER, PROC_NULL, 0);
        AppendUint(buffer + RPC_VERSION_OFFSET, 0);
        TEST_ASSERT(TestInvalidCall(0x9e0f824c, callSize), __LINE__, "TestInvalidCall failed");
    }
    // Test invalid rpc version 1
    {
        UINT callSize = SetupCall(RPC_PROGRAM_PORTMAP_NETWORK_ORDER, _2_NETWORK_ORDER, PROC_NULL, 0);
        AppendUint(buffer + RPC_VERSION_OFFSET, 1);
        TEST_ASSERT(TestInvalidCall(0x97e9b9a7, callSize), __LINE__, "TestInvalidCall failed");
    }
    // Test invalid rpc version 3
    {
        UINT callSize = SetupCall(RPC_PROGRAM_PORTMAP_NETWORK_ORDER, _2_NETWORK_ORDER, PROC_NULL, 0);
        AppendUint(buffer + RPC_VERSION_OFFSET, 3);
        TEST_ASSERT(TestInvalidCall(0x1fb8a732, callSize), __LINE__, "TestInvalidCall failed");
    }

    //
    // Test program unavailable
    //
    {
        UINT callSize = SetupCall(0x819a, 0, 0, 0);
        TEST_ASSERT(TestCall(&conn, 0xb8932af1, callSize, 1, RPC_REPLY_ACCEPT_STATUS_PROG_UNAVAIL),
            __LINE__, "test failed");
    }
    //
    // Test program version mismatch
    //
    {
        UINT callSize = SetupCall(RPC_PROGRAM_PORTMAP_NETWORK_ORDER, 0xf12, PROC_NULL, 0);
        TEST_ASSERT(TestCall(&conn, 0xf0188a8f, callSize,
            3, RPC_REPLY_ACCEPT_STATUS_PROG_MISMATCH, 2, 2), __LINE__, "test failed");
    }
    {
        UINT callSize = SetupCall(RPC_PROGRAM_NFS_NETWORK_ORDER, 0x4289, PROC_NULL, 0);
        TEST_ASSERT(TestCall(&conn, 0x23547890, callSize,
            3, RPC_REPLY_ACCEPT_STATUS_PROG_MISMATCH, 3, 3), __LINE__, "test failed");
    }

    //
    // Test procedure unavailable
    //
    {
        UINT callSize = SetupCall(RPC_PROGRAM_PORTMAP_NETWORK_ORDER, _2_NETWORK_ORDER, 0x9184, 0);
        TEST_ASSERT(TestCall(&conn, 0x5918f8a9, callSize,
            1, RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL), __LINE__, "test failed");
    }
    {
        UINT callSize = SetupCall(RPC_PROGRAM_NFS_NETWORK_ORDER, _3_NETWORK_ORDER, 0x4f20, 0);
        TEST_ASSERT(TestCall(&conn, 0x1948f910, callSize,
            1, RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL), __LINE__, "test failed");
    }

    // TODO: test a handle length of 0, some other value like 10, test max value of 64, and a value that is too large
    {
        UINT callSize = SetupCall(RPC_PROGRAM_NFS_NETWORK_ORDER, _3_NETWORK_ORDER, NFS3_PROC_FSINFO_NETWORK_ORDER,
            2, 4, 0x84398184);
        TEST_ASSERT(TestCall(&conn, 0xa819f9e8, callSize,
            3, RPC_REPLY_ACCEPT_STATUS_SUCCESS, NFS3_ERROR_BADHANDLE, 0), __LINE__, "test failed");
    }

    return TEST_SUCCESS;
}



int PerformanceTest(unsigned runCount, unsigned loopCount)
{
    volatile char buffer[4];

    LARGE_INTEGER frequency;
    if(!QueryPerformanceFrequency(&frequency))
    {
        LOG_ERROR("QueryPerformanceFrequency failed (e=%d)", GetLastError());
        return 1; // fail
    }
    LOG("frequency %llu", frequency.QuadPart);

    LARGE_INTEGER before;
    LARGE_INTEGER after;

    for(unsigned run = 0; run < runCount; run++)
    {
        if(!QueryPerformanceCounter(&before))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        for(unsigned loop = 0; loop < loopCount; loop++)
        {
            AppendUint((char*)buffer, loop);
        }
        if(!QueryPerformanceCounter(&after))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        LOG("AppendUint: %llu", (after.QuadPart-before.QuadPart));
        if(!QueryPerformanceCounter(&before))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        for(unsigned loop = 0; loop < loopCount; loop++)
        {
            *((volatile UINT*)buffer) = htonl(loop);
        }
        if(!QueryPerformanceCounter(&after))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        LOG("htonl     : %llu", (after.QuadPart-before.QuadPart));
        if(!QueryPerformanceCounter(&before))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        for(unsigned loop = 0; loop < loopCount; loop++)
        {
            SET_UINT(buffer, loop);
        }
        if(!QueryPerformanceCounter(&after))
        {
            LOG_ERROR("QueryPerformanceCounter failed (e=%d)", GetLastError());
            return 1;
        }
        LOG("rawset    : %llu", (after.QuadPart-before.QuadPart));
    }
    return TEST_SUCCESS;
}

int main(int argc, char* argv[])
{
    //PerformanceTest(3, 1000000000);

    Wsa wsa;
    if(wsa.error)
    {
        LOG_ERROR("WSAStartup failed (returned %d)", wsa.error);
        return 1;
    }

    int result = run();

    if(result == TEST_SUCCESS)
    {
        printf("SUCCESS\r\n");
        return 0;
    }
    else
    {
        printf("ERROR\r\n");
        return 1; // fail
    }
}