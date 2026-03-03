#include "socket.hpp"

#include <sys/socket.h>
#include <sys/un.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "util.hpp"

std::unique_ptr<SocketSender> g_sender;

Result<> SocketSender::Start()
{
#ifdef __linux__
    m_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_sock < 0)
        return Err("Failed to open socket stream: " + std::string(strerror(errno)));

    sockaddr_un serv_addr{};
    serv_addr.sun_family = AF_UNIX;

    strncpy(serv_addr.sun_path, (get_runtime_dir() / "oshot.sock").c_str(), 107);

    if (connect(m_sock, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)))
        return Err("Failed to connect to launcher: " + std::string(strerror(errno)));
#endif
    return Ok();
}

Result<> SocketSender::Send(const std::string& text)
{
    return Send(SendMsg::Text, text.c_str(), text.size());
}

Result<> SocketSender::Send(SendMsg msg, const void* src, size_t size)
{
#ifdef __linux__
    if (!src || size == 0)
        return Err("No data to send");

    char type;
    switch (msg)
    {
        case SendMsg::Text:  type = 'T'; break;
        case SendMsg::Image: type = 'I'; break;
        default:             return Err("Unknown message to send");
    }

    if (size > UINT32_MAX)
        return Err("Message size too big");

    if (send(m_sock, &type, 1, 0) != 1)
        return Err("Failed to send message type: " + std::string(strerror(errno)));

    uint32_t net_len = htonl(static_cast<uint32_t>(size));  // network byte order
    if (send(m_sock, reinterpret_cast<const char*>(&net_len), sizeof(net_len), 0) != sizeof(net_len))
        return Err("Failed to send message size: " + std::string(strerror(errno)));

    const char* buf = reinterpret_cast<const char*>(src);

    size_t           sent       = 0;
    constexpr size_t chunk_size = 64 * 1024;  // 64KB
    while (sent < size)
    {
        size_t  remaining = size - sent;
        size_t  chunk     = std::min(remaining, chunk_size);
        ssize_t n         = send(m_sock, buf + sent, chunk, 0);
        if (n <= 0)
            return Err("Failed to send all message data");

        sent += n;
    }
#endif
    return Ok();
}

void SocketSender::Close()
{
#ifdef __linux__
    if (m_sock >= 0)
    {
        close(m_sock);
        m_sock = -1;
    }
#endif
}
