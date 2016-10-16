#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "SelectServer.h"

class ScopedCriticalSectionLock
{
  private:
    CRITICAL_SECTION* criticalSection;
  public:
    ScopedCriticalSectionLock(CRITICAL_SECTION* criticalSection)
        : criticalSection(criticalSection)
    {
        EnterCriticalSection(criticalSection);
    }
    ~ScopedCriticalSectionLock()
    {
        LeaveCriticalSection(criticalSection);
    }
};

// Assumption: sock->s       != INVALID_SOCKET
// Assumption: sock->handler != NULL
// Assumption: sock->flags   != 0 || sock->timeout != SelectSock::INF
// Returns: non-zero if thread is full
BOOL SynchronizedSelectServer::TryAddSock(const SelectSock& sock)
{
    if(server->socksReserved >= SELECT_THREAD_CAPACITY) {
        return TRUE; // server is full
    }
    server->socks[server->socksReserved] = sock;
    server->socksReserved++;
    return FALSE; // server is no full
}

// Returns: index on success, count on error
// hint: contains the guess of where the next socket will be
static u_int Find(u_int count, const SelectSock socks[], SOCKET so, u_int* outHint)
{
    u_int initialHint = *outHint;
    u_int i;

    for(i = initialHint; i < count; i++) {
        if(so == socks[i].so) {
            *outHint = i + 1; // increment for next time
            return i;
        }
    }

    // If we get to this point in the function
    // then the select sockets were out of order so the
    // hint mechanism didn't quite work.  Maybe we could log
    // when this happens to determine whether or not there is
    // a better mechanism to find the sockets.

    for(i = 0; i < initialHint; i++) {
        if(so == socks[i].so) {
            *outHint = i + 1; // increment for next time
            return i;
        }
    }

    return count; // ERROR
}

// The order of these values is important.
// The SelectServer checks event in this order:
//   1. Error Events
//   2. Write Events
//   3. Read Events
//   4. Timeout Events
// Once an event is handled, it's handler state is set.  If any more events were raised
// in the same select call, then they will only be called if their handler state is >= the current state.
// This means if the error handler is called, no other handler will be called.
// Also, if either the read or write handler is called, the timeout handler will not be called, however,
// the read and write handlers could both be called.
enum HandledState : BYTE {
    CALLED_TIMEOUT_OR_NO_HANDLER = 0,
    CALLED_READ_OR_WRITE_HANDLER = 1,
    CALLED_ERROR_HANDLER         = 2,
};

struct SetProperties {

    const char* name;
    PopReason reason;
    BYTE setFlag;
    HandledState handled;
};

SetProperties setProps[3] = {
    {"read",  POP_REASON_READ , SelectSock::READ  , CALLED_READ_OR_WRITE_HANDLER}, // read set
    {"write", POP_REASON_WRITE, SelectSock::WRITE , CALLED_READ_OR_WRITE_HANDLER}, // write set
    {"error", POP_REASON_ERROR, SelectSock::ERROR_, CALLED_ERROR_HANDLER        },// error set
};

unsigned sprintsets(char* buffer, const SockSet sets[])
{
    unsigned off = 0;

    for(BYTE setIndex = 0; setIndex < 3; setIndex++) {
        if(sets[setIndex].count > 0) {
            u_int sockIndex;
            off += sprintf(buffer + off, " %s=", setProps[setIndex].name);
            for(sockIndex = 0; sockIndex < sets[setIndex].count; sockIndex++) {
                if(sockIndex > 0) {
                    buffer[off++] = ',';
                }
                off += sprintf(buffer + off, "%d", sets[setIndex].array[sockIndex]);
            }
        }
    }
    return off;
}


DWORD SelectServer::Run(char* sharedBuffer, size_t sharedBufferSize)
{
    u_int activeSockCount = 0;
    SockSet sets[3];

    // Tracks whether or not a socket was already handled after a select call
    // We could include this as a field in the private data, but I don't do this
    // because this data needs to be "zeroed" in every iteration of select, so
    // in order to use the ZeroMemory function I include it an it's own array.
    BYTE handled[SELECT_THREAD_CAPACITY];

    // socks is used so much, I'm explicitly caching the pointer to it on the
    // stack, this is also shadowing the member variable socks so
    // it will default to the pointer on the stack
    SelectSock* socks = this->socks;

    // Used to know whether there are any sockets to remove
    // without having to traverse the whole list of sockets.
    // Also allows the algorithm to know where to start packing sockets
    // without having to check all the previous sockets
    // TODO: rename minSockToRemove to removeStartIndex
    u_int minSockToRemove = SELECT_THREAD_CAPACITY;


    while(1)
    {
        //
        // Add/Remove sockets and check for shutdown inside a lock
        //
        {
            ScopedCriticalSectionLock lock(&criticalSection);

            // Add any new reserved sockets
            if(socksReserved > activeSockCount)
            {
                SELECT_SERVER_LOG("Adding %d sockets (%d sockets total)",
                                  socksReserved - activeSockCount, socksReserved);
                do
                {
                    // setup timeoutTickCount if there is a timeout
                    if(socks[activeSockCount].timeout != SelectSock::INF)
                    {
                        socks[activeSockCount].timeoutTickCount = GetTickCount() + socks[activeSockCount].timeout;
                        SELECT_SERVER_LOG("[SelectThreadTimeout] s=%d timeout=%d millis time=%d",
                                          socks[activeSockCount].so, socks[activeSockCount].timeout, socks[activeSockCount].timeoutTickCount);
                    }
                    SELECT_SERVER_LOG("added socket (s=%d)", socks[activeSockCount].so);

                    activeSockCount++;
                } while(activeSockCount < socksReserved);
            }

            // Remove sockets that have no flags and an infinite timeout, then pack the rest
            if(minSockToRemove != SELECT_THREAD_CAPACITY)
            {
                u_int packFrom;

                SELECT_SERVER_LOG("removing socket (s=%d)", socks[minSockToRemove].so);

                for(packFrom = minSockToRemove + 1; packFrom < activeSockCount; packFrom++)
                {
                    if((socks[packFrom].flags & SelectSock::ALL) == 0 && socks[packFrom].timeout == SelectSock::INF)
                    {
                        SELECT_SERVER_LOG("removing socket (s=%d)", socks[packFrom].so);
                        // remove it by skipping it
                        // TODO: maybe there should be a CloseOnRemoval option for this server?
                        // shutdown and close?
                    }
                    else
                    {
                        socks [minSockToRemove] = socks [packFrom];
                        //psocks[minSockToRemove] = psocks[packFrom];
                        minSockToRemove++;
                    }
                }
                SELECT_SERVER_LOG("Removed %d sockets (%d sockets total)", activeSockCount - minSockToRemove, minSockToRemove);
                activeSockCount = minSockToRemove;
                socksReserved = minSockToRemove;

                minSockToRemove = SELECT_THREAD_CAPACITY; // means no more socks to remove
            }
            if(flags & STOP_FLAG)
            {
                // TODO: Closing sockets on shutdown should be an option
                /*
                for(u_int i = 0; i < activeSockCount; i++) {
                    shutdown(socks[i].so, SD_BOTH);
                    closesocket(socks[i].so);
                }
                */
                break;
            }
        } // End of critical section lock

        if(activeSockCount == 0)
        {
            SELECT_SERVER_LOG("no more sockets");
            break;
        }

        //
        // Setup select call, add sockets to sets and get minimum timeout
        //
        sets[0].count = 0;
        sets[1].count = 0;
        sets[2].count = 0;

        DWORD minTimeDiff = 0xFFFFFFFF;

        {
            DWORD now;
            for(u_int i = 0; i < activeSockCount; i++)
            {
                //_tprintf(TEXT("[%d] s = %d"), i, socks[i]);
                if(socks[i].flags & SelectSock::READ)
                {
                    sets[0].Add(socks[i].so);
                }
                if(socks[i].flags & SelectSock::WRITE)
                {
                    sets[1].Add(socks[i].so);
                }
                if(socks[i].flags & SelectSock::ERROR_)
                {
                    sets[2].Add(socks[i].so);
                }
                if(socks[i].timeout != SelectSock::INF)
                {
                    if(minTimeDiff == 0xFFFFFFFF)
                    {
                        // Case: the first socket with a timeout in this loop
                        now = GetTickCount();

                        minTimeDiff = socks[i].timeoutTickCount - now;
                        if(minTimeDiff >= 0x7FFFFFFF) {
                            minTimeDiff = 0;
                        }
                        SELECT_SERVER_LOG("[SelectThreadTimeout] s=%d timediff=%d millis (time=%d,now=%d)",
                                          socks[i].so, minTimeDiff, socks[i].timeoutTickCount, now);
                    }
                    else
                    {
                        // Case: NOTE the first socket with a timeout in this loop
                        DWORD diff = socks[i].timeoutTickCount - now;
                        if(diff >= 0x7FFFFFFF) {
                            diff = 0;
                        }
                        SELECT_SERVER_LOG("[SelectThreadTimeout] s=%d timediff=%d millis (time=%d,now=%d)",
                                          socks[i].so, diff, socks[i].timeoutTickCount, now);
                        if(diff < minTimeDiff) {
                            minTimeDiff = diff;
                        }
                    }
                }
            }
        }

        // Setup the timeout
        struct timeval timeout;
        if(minTimeDiff != 0xFFFFFFFF)
        {
            if(minTimeDiff == 0)
            {
                SELECT_SERVER_LOG("[SelectThreadTimeout] Timeout of 0!");
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
            }
            else
            {
                timeout.tv_sec  = minTimeDiff / 1000;// seconds
                timeout.tv_usec = (minTimeDiff % 1000) * 1000; // microseconds
                SELECT_SERVER_LOG("[SelectThreadTimeout] Timeout (%d millis) (%d secs, %d usecs)",
                                  minTimeDiff, timeout.tv_sec, timeout.tv_usec);
            }
        }

#ifdef SELECT_THREAD_VERBOSE
        {
            // TODO: use snprintf, or allocate a buffer with alloca
            unsigned off = 0;
            off  = sprintf((char*)sharedBuffer, "select");
            off += sprintsets((char*)sharedBuffer + off, sets);
            SELECT_SERVER_LOG("%s", sharedBuffer);
        }
#endif
        //_tprintf(TEXT("%s select(read=%d,write=%d,err=%d)..."), thread->logID, sets[0].count, sets[1].count, sets[2].count);

        int selectCount = select(0, (fd_set*)&sets[0], (fd_set*)&sets[1], (fd_set*)&sets[2],
                             (minTimeDiff == 0xFFFFFFFF) ? NULL : &timeout);
        if(selectCount < 0)
        {
            SELECT_SERVER_LOG("Error: select failed (e=%d), stopping thread", GetLastError());
            return 1; // fail
        }

        ZeroMemory(handled, sizeof(handled[0]) * activeSockCount);

        //
        // Handle Popped Sockets
        //
        if(selectCount > 0)
        {

#ifdef SELECT_THREAD_VERBOSE
            {
                unsigned off = 0;
                off  = sprintf((char*)sharedBuffer, "popped");
                off += sprintsets((char*)sharedBuffer + off, sets);
                SELECT_SERVER_LOG("%s", sharedBuffer);
            }
#endif

            for(unsigned char setIndex = 2;; setIndex--)
            {
                // The hint keeps track of where the last popped socket was found,
                // it used to determine where to start searching for the next socket.
                // If select keeps the sockets in order, then the hint will find the sockets
                // in the most efficient way possible.
                u_int hint = 0;

                for(u_int i = 0; i < sets[setIndex].count; i++)
                {
                    SOCKET so = sets[setIndex].array[i];

                    //_tprintf(TEXT("searching for s = %d (index = %d)..."), s, i);
                    u_int sockIndex = Find(activeSockCount, socks, so, &hint);

                    if(sockIndex == activeSockCount)
                    {
                        //
                        // This is probably a code bug, in either the application or the
                        // implementation of select.
                        //
                        //_tprintf(TEXT("s = %d is missing"), s);
                        // todo: handle error
                        continue;
                    }

                    //
                    // NOTE: this code is pretty much an exact copy of the handler code in the timeout section
                    //
                    if(handled[sockIndex] > setProps[setIndex].handled)
                    {
                        // Socket already handled for this select iteration
                        continue;
                    }

                    if((socks[sockIndex].flags & setProps[setIndex].setFlag) == 0)
                    {
                        // This could happen if another handler was called on this select
                        // iteration and removed the select flag on the socket for this set
                        continue;
                    }

                    //_tprintf(TEXT("Calling handler %p for s = %d..."), socks[sockIndex].handler, s);
                    socks[sockIndex].handler(SynchronizedSelectServer(this), &socks[sockIndex], setProps[setIndex].reason, sharedBuffer);
                    handled[sockIndex] = setProps[setIndex].handled;

                    if(socks[sockIndex].timeout != SelectSock::INF) {
                        // Check for any timeout
                        socks[sockIndex].timeoutTickCount = GetTickCount() + socks[sockIndex].timeout;
                        SELECT_SERVER_LOG("[SelectThreadTimeout] s=%d timeout=%d millis time=%d",
                                          socks[sockIndex].so, socks[sockIndex].timeout, socks[sockIndex].timeoutTickCount);
                    } else if( (socks[sockIndex].flags & SelectSock::ALL) == 0) {
                        // Set remove socket index so we know there are sockets to remove
                        if(sockIndex < minSockToRemove) {
                            // Set the minimum index to remove to assist the removal algorithm
                            minSockToRemove = sockIndex;
                        }
                    }
                }

                if(setIndex == 0)
                {
                    break;
                }
            }
        }

        //
        // Handle Timeouts
        //
        if(minTimeDiff != 0xFFFFFFFF)
        {
            DWORD diff;
            DWORD now = GetTickCount();

            for(u_int sockIndex = 0; sockIndex < activeSockCount; sockIndex++)
            {
                if(socks[sockIndex].timeout != SelectSock::INF)
                {
                    // check if the timeout has been reached
                    {
                        DWORD diff = socks[sockIndex].timeoutTickCount - now;
                        if(diff < 0x7FFFFFFF) {
                            SELECT_SERVER_LOG("[SelectThreadTimeout] s=%d still has %d milliseconds before timeout", socks[sockIndex].so, diff);
                            continue;
                        }   
                    }

                    //
                    // NOTE: this code is pretty much an exact copy of the handler code in the popped sets section
                    //
                    if(handled[sockIndex]) {
                        // Socket already handled for this select iteration
                        continue;
                    }

                    //_tprintf(TEXT("Calling handler %p for s = %d..."), socks[sockIndex].handler, s);
                    socks[sockIndex].handler(this, &socks[sockIndex], POP_REASON_TIMEOUT, sharedBuffer);

                    // We don't need to mark it as handled because there are no more handler calls
                    //handled[sockIndex] = setProps[setIndex].handled;

                    if(socks[sockIndex].timeout != SelectSock::INF) {
                        // Check for any timeout
                        socks[sockIndex].timeoutTickCount = GetTickCount() + socks[sockIndex].timeout;
                        SELECT_SERVER_LOG("[SelectThreadTimeout] s=%d timeout=%d millis time=%d",
                                          socks[sockIndex].so, socks[sockIndex].timeout, socks[sockIndex].timeoutTickCount);
                    } else if( (socks[sockIndex].flags & SelectSock::ALL) == 0) {
                        // Set remove socket index so we know there are sockets to remove
                        if(sockIndex < minSockToRemove) {
                            // Set the minimum index to remove to assist the removal algorithm
                            minSockToRemove = sockIndex;
                        }
                    }
                }
            }
        }

    } // end of while loop

    return 0;
}
