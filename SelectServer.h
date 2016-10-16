#pragma once

#include <SelectServerParams.h>

// Application can override the socket capacity for a single thread
#ifndef SELECT_THREAD_CAPACITY
#define SELECT_THREAD_CAPACITY 64
#endif

// Application can define this to enable verbose logging
//#define SELECT_THREAD_VERBOSE

// Application can define their own log callback
#ifndef SELECT_SERVER_LOG
#define SELECT_SERVER_LOG(fmt, ...)
#endif

struct SockSet
{
    u_int count;
    SOCKET array[SELECT_THREAD_CAPACITY];
    void Add(SOCKET so)
    {
        array[count++] = so;
    }
};

// After a select call a socket could be popped for a number of reasons.
// If it pops in the socket error set (exceptfds) it's handler will be called
// with POP_REASON_ERROR and will not be called again until the next select.

// If it pops with READ or WRITE, it's handler will be called once for each
// and it's handler will not be called again until the next select.

// The last case is the timeout case, where the handler will be called with POP_REASON_TIMEOUT.
enum PopReason
{
  POP_REASON_TIMEOUT = 0,
  POP_REASON_READ    = 1,
  POP_REASON_WRITE   = 2,
  POP_REASON_ERROR   = 3,
};

class SelectServer;
class SelectSock;
class SynchronizedSelectServer;
class LockedSelectServer;

// TODO: I might be able to pass the LockedSelectServer to the callback, but I
//       don't want it to enter/exit the critical sections
typedef void (*SelectSockHandler)(SynchronizedSelectServer server, SelectSock* sock, PopReason reason, char* sharedBuffer);

// Note: This data should only ever be modified by the select
//       thread itself.  If it isn't, then all types of synchronization
//       will need to occur.
class SelectSock
{
    friend class SelectServer;
  public:
    enum Flags : BYTE {
        NONE   = 0x00,
        READ   = 0x01,
        WRITE  = 0x02,
        ERROR_ = 0x04,
        ALL    = (READ|WRITE|ERROR_),
    };
    enum _Timeout : DWORD {
        INF = 0xFFFFFFFF,
    };

    SOCKET so;
    void* user; // A user pointer
  private:
    SelectSockHandler handler;
    // Use SelectSock::INF (which will be 0xFFFFFFFF) to indicate no timeout.
    // Otherwise, this value indicates the number of milliseconds to wait before calling the
    // handler again.
    DWORD timeout;
    // The tick count that indicates whether the socket is ready to time out
    DWORD timeoutTickCount;
    Flags flags;
    SelectSock()
    {
    }
  public:
    SelectSock(SOCKET so, void* user, SelectSockHandler handler, Flags flags, DWORD timeout)
        : so(so), user(user), handler(handler), flags(flags), timeout(timeout)
    {
    }

    void UpdateHandler(SelectSockHandler handler)
    {
        this->handler = handler;
    }
    void UpdateEventFlags(Flags flags)
    {
        this->flags = flags;
    }
    void UpdateTimeout(DWORD millis)
    {
        this->timeout = millis;
    }
};

class SelectServer
{
    friend class SynchronizedSelectServer;
    friend class LockedSelectServer;
  private:
    CRITICAL_SECTION criticalSection;

    // NOTE: only read/modify inside critical section
    // tracks how many sockets are being used plus how many
    // socks will be added after the next select
    // If this is set to 0, it means the SelectThread should shutdown
    u_int socksReserved;

    BYTE flags;

    // All the active sockets will be packed to the start
    // of the array, and any other threads can add sockets
    // back adding then after the active sockets and incrementing
    // the socksReserved field (inside the critical section of cource)
    SelectSock socks[SELECT_THREAD_CAPACITY];

  public:
    enum Flags {
        STOP_FLAG = 0x01,
    };

    SelectServer() : socksReserved(0), flags(0)
    {
        InitializeCriticalSection(&criticalSection);
    }
    ~SelectServer()
    {
        DeleteCriticalSection(&criticalSection);
    }
    DWORD Run(char* sharedBuffer, size_t sharedBufferSize);
};


class SynchronizedSelectServer
{
    friend class SelectServer;
  protected:
    SelectServer* server;
    SynchronizedSelectServer(SelectServer* server) : server(server)
    {
    }
  public:
    void SetStopFlag()
    {
        server->flags |= SelectServer::STOP_FLAG;
    }
    u_int AvailableSocks()
    {
        return SELECT_THREAD_CAPACITY - server->socksReserved;
    }
    BOOL TryAddSock(const SelectSock& sock);
};
class LockedSelectServer : public SynchronizedSelectServer
{
  public:
    LockedSelectServer(SelectServer* server) : SynchronizedSelectServer(server)
    {
        EnterCriticalSection(&server->criticalSection);
        SELECT_SERVER_LOG("locked");
    }
    ~LockedSelectServer()
    {
        SELECT_SERVER_LOG("unlocked");
        LeaveCriticalSection(&server->criticalSection);
    }
};
