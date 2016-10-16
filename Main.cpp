#include <winsock2.h>
#include <stdio.h>

#include "Common.h"
#include "NfsServer.h"

int main(int argc, char* argv[])
{
    Wsa wsa;
    if(wsa.error)
    {
        LOG_ERROR("WSAStartup failed (returned %d)", wsa.error);
        return 1;
    }

    return RunNfsServer();
}