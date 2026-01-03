#include "socket.hpp"

#include <string>

bool SocketSender::Start(int port)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    m_sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    m_failed = (connect(m_sock, reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) == -1);
    return !m_failed;
}

bool SocketSender::Send(const std::string& text)
{
    if (text.empty())
        return false;

    return (send(m_sock, text.c_str(), text.length(), 0) != -1);
}

SocketSender::~SocketSender()
{
    close(m_sock);
#ifdef _WIN32
    WSACleanup();
#endif
}
