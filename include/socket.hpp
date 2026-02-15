#ifndef _SOCKET_HPP_
#define _SOCKET_HPP_

#include <memory>
#include <string>

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

    bool Start(int port = 6015);
    bool Send(const std::string& text);
    bool Send(SendMsg msg, const void* src, size_t size);
    bool IsFailed() { return m_failed; }

    void Close();

private:
    int  m_sock{};
    bool m_failed{};
};

extern std::unique_ptr<SocketSender> g_sender;

#endif  // !_SOCKET_HPP_
