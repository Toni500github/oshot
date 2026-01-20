#ifndef _SOCKET_HPP_
#define _SOCKET_HPP_

#include <memory>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
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

    bool Start(int port = 6015);
    bool Send(const std::string& text);
    bool Send(SendMsg msg, const void* src, size_t size);
    bool IsFailed() { return m_failed; }

    void Close();

private:
#ifdef _WIN32
    size_t m_sock{};
#else
    int m_sock{};
#endif
    bool m_failed{};
};

extern std::unique_ptr<SocketSender> sender;

#endif  // !_SOCKET_HPP_
