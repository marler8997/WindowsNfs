#pragma once

#define LOG_ERROR(fmt,...) printf("Error: " fmt "\r\n",##__VA_ARGS__)
#define LOG(fmt,...) printf(fmt "\r\n",##__VA_ARGS__)

class Wsa
{
  public:
    int error;
    Wsa()
    {
        WSADATA data;
        error = WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~Wsa()
    {
        if(error == 0) {
            WSACleanup();
        }
    }
};