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

void AppendUint(char* buffer, UINT value)
{
    buffer[0] = (char)(value >> 24);
    buffer[1] = (char)(value >> 16);
    buffer[2] = (char)(value >> 8);
    buffer[3] = (char)value;
}