#ifndef _SOCKET_HPP_
#define _SOCKET_HPP_

#include <memory>
#include <string>

#include "util.hpp"

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

enum class SendMsg
{
    Text,
    Image,
};

class SocketSender
{
public:
    ~SocketSender() { Close(); };

    Result<> Start();
    Result<> Send(const std::string& text);
    Result<> Send(SendMsg msg, const void* src, size_t size);

    void Close();

private:
    int m_sock = -1;
};

extern std::unique_ptr<SocketSender> g_sender;

#endif  // !_SOCKET_HPP_
