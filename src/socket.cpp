#include "socket.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "util.hpp"

bool SocketSender::Start(int port)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return false;
#endif

    m_sock = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (m_sock == INVALID_SOCKET)
        return false;
#else
    if (m_sock < 0)
        return false;
#endif

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0)
        return false;

    m_failed = (connect(m_sock, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0);

#ifdef _WIN32
    if (m_failed)
        error("connect to launcher failed: {}", WSAGetLastError());
#else
    if (m_failed)
        error("connecting to launcher failed: {}", strerror(errno));
#endif

    return !m_failed;
}

bool SocketSender::Send(const std::string& text)
{
    if (text.empty())
        return false;

    return Send(SendMsg::Text, text.c_str(), text.size());
}

bool SocketSender::Send(SendMsg msg, const void* src, size_t size)
{
    if (!src || size < 2)
        return false;

    char type;
    switch (msg)
    {
        case SendMsg::Text:  type = 'T'; break;
        case SendMsg::Image: type = 'I'; break;
        default:             return false;
    }

    if (size > UINT32_MAX)
        return false;

    uint32_t net_len = htonl(static_cast<uint32_t>(size));  // network byte order
    if (send(m_sock, &type, 1, 0) != 1)
        return false;

    if (send(m_sock, reinterpret_cast<const char*>(&net_len), sizeof(net_len), 0) != sizeof(net_len))
        return false;

    const char* buf = reinterpret_cast<const char*>(src);

    size_t           sent       = 0;
    constexpr size_t CHUNK_SIZE = 64 * 1024;  // 64KB
    while (sent < size)
    {
        size_t remaining = size - sent;
        size_t chunk     = std::min(remaining, CHUNK_SIZE);
        int    n         = send(m_sock, buf + sent, chunk, 0);
        if (n <= 0)
            return false;

        sent += n;
    }

    return true;
}

void SocketSender::Close()
{
#ifdef _WIN32
    if (m_sock != INVALID_SOCKET)
    {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
#else
    if (m_sock >= 0)
    {
        close(m_sock);
        m_sock = -1;
    }
#endif
}
