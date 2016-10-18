#pragma once

#define LOG_ERROR(fmt,...) printf("Error: " fmt "\r\n",##__VA_ARGS__)
#define LOG(fmt,...)       printf(fmt "\r\n",##__VA_ARGS__)

#define LOG_NET(fmt,...)   //printf("[NET] " fmt "\r\n",##__VA_ARGS__)
#define LOG_RPC(fmt,...)   //printf("[RPC] " fmt "\r\n",##__VA_ARGS__)

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

struct String
{
    char* ptr;
    UINT length;
    String() : ptr(NULL), length(0)
    {
    }
    String(char* ptr, UINT length) : ptr(ptr), length(length)
    {
    }
    bool Equals(String other)
    {
        return this->length == other.length &&
          memcmp(this->ptr, other.ptr, other.length) == 0;
    }
};