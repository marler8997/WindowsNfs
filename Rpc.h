#pragma once

#define RPC_LAST_FRAGMENT_FLAG 0x80000000

#define RPC_MESSAGE_TYPE_CALL  0
#define RPC_MESSAGE_TYPE_REPLY 1

// NOTE: uncommenting this causes a problem in the microsoft header file
//#define RPC_VERSION         2

#define RPC_PROGRAM_PORTMAP 100000
#define RPC_PROGRAM_NFS     100003

#define RPC_REPLY_ACCEPTED 0
#define RPC_REPLY_DENIED   1

#define RPC_REPLY_ACCEPT_STATUS_SUCCESS       0
#define RPC_REPLY_ACCEPT_STATUS_PROG_UNAVAIL  1 // program not found
#define RPC_REPLY_ACCEPT_STATUS_PROG_MISMATCH 2 // program does not support this version
#define RPC_REPLY_ACCEPT_STATUS_PROC_UNAVAIL  3 // program doesn't support procedure
#define RPC_REPLY_ACCEPT_STATUS_GARBAGE_ARGS  4 // program can't decode params

#define RPC_REJECT_STATUS_RPC_MISMATCH 0 // rpc version number != 2
#define RPC_REJECT_STATUS_AUTH_ERROR   1 // can't authenticate caller

#define RPC_AUTH_FLAVOR_NULL 0

UINT ParseUint(char* buffer);
void AppendUint(char* buffer, UINT value);

#define SET_UINT(buffer, value) *((UINT*)(buffer)) = value
#define GET_UINT(buffer) *((UINT*)(buffer))

// This should always be 0, no need for a network order
#define PROC_NULL 0

#define PORTMAP_PROC_GETPORT 3

//
// Network Order Constants
//
#if LITTLE_ENDIAN
    #define ONE_NETWORK_ORDER                                   0x01000000
    #define TWO_NETWORK_ORDER                                   0x02000000
    #define THREE_NETWORK_ORDER                                 0x03000000
    #define FOUR_NETWORK_ORDER                                  0x04000000

    #define RPC_PROGRAM_PORTMAP_NETWORK_ORDER                   0xA0860100
    #define RPC_PROGRAM_NFS_NETWORK_ORDER                       0xA3860100

#elif BIG_ENDIAN
    #define ONE_NETWORK_ORDER                                   0x00000001
    #define TWO_NETWORK_ORDER                                   0x00000002
    #define THREE_NETWORK_ORDER                                 0x00000003
    #define FOUR_NETWORK_ORDER                                 0x00000004

    #define RPC_PROGRAM_PORTMAP_NETWORK_ORDER                   0x000186A0
    #define RPC_PROGRAM_NFS_NETWORK_ORDER                       0x000186A3
#else
    #error Need to define LITTLE_ENDIAN or BIG_ENDIAN
#endif

#define RPC_MESSAGE_TYPE_CALL_NETWORK_ORDER     0
#define RPC_MESSAGE_TYPE_REPLY_NETWORK_ORDER    ONE_NETWORK_ORDER

#define RPC_VERSION_NETWORK_ORDER               TWO_NETWORK_ORDER

#define RPC_REPLY_ACCEPTED_NETWORK_ORDER        0
#define RPC_REPLY_DENIED_NETWORK_ORDER          ONE_NETWORK_ORDER

#define RPC_REPLY_STATUS_ACCEPTED_NETWORK_ORDER 0x00000000
#define RPC_AUTH_FLAVOR_NULL_NETWORK_ORDER      0x00000000
#define NFS3_PROC_NULL_NETWORK_ORDER            0x00000000

#define RPC_REPLY_ACCEPT_STATUS_PROG_MISMATCH_NETWORK_ORDER TWO_NETWORK_ORDER



