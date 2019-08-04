#include <windows.h>

#include "Rpc.h"
#include <stdio.h>
UINT ParseUint(char* buffer)
{
    return (unsigned char)buffer[0] << 24 |
           (unsigned char)buffer[1] << 16 |
           (unsigned char)buffer[2] <<  8 |
           (unsigned char)buffer[3]       ;
}
UINT64 ParseUint64(char* buffer)
{
    return (UINT64)buffer[0] << 56 |
           (UINT64)buffer[1] << 48 |
           (UINT64)buffer[2] << 40 |
           (UINT64)buffer[3] << 32 |
           (UINT64)buffer[4] << 24 |
           (UINT64)buffer[5] << 16 |
           (UINT64)buffer[6] <<  8 |
           (UINT64)buffer[7]       ;
}

void AppendUint(char* buffer, UINT value)
{
    buffer[0] = (char)(value >> 24);
    buffer[1] = (char)(value >> 16);
    buffer[2] = (char)(value >> 8);
    buffer[3] = (char)value;
}