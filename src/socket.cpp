#include "socket.hpp"

#include <string>

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
        error("connect failed: %d\n", WSAGetLastError());
#else
    if (m_failed)
        perror("connect failed");
#endif

    return !m_failed;
}

bool SocketSender::Send(const std::string& text)
{
    if (text.empty())
        return false;

    constexpr char type = 'T';
    if (send(m_sock, &type, 1, 0) != 1)
        return false;

    return (send(m_sock, text.c_str(), text.length(), 0) != -1);
}

bool SocketSender::Send(SendMsg msg, const void* src, size_t size)
{
    if (!src)
        return false;

    char type;
    switch (msg)
    {
        case SendMsg::COPY_TEXT:  type = 'T'; break;
        case SendMsg::COPY_IMAGE: type = 'I'; break;
        default:                  return false;
    }

    uint32_t len = htonl(static_cast<uint32_t>(size));  // network byte order

    if (send(m_sock, &type, 1, 0) != 1)
        return false;

    if (send(m_sock, reinterpret_cast<const char*>(&len), 4, 0) != 4)
        return false;

#ifdef _WIN32
    const char* buf = reinterpret_cast<const char*>(src);
#else
    const char* buf = static_cast<const char*>(src);
#endif

    size_t sent = 0;
    while (sent < size)
    {
        int n = send(m_sock, buf + sent, static_cast<int>(size - sent), 0);
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
