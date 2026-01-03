#include <string>
#ifdef _WIN32
#include <ws2tcpip.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#define close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

class SocketSender
{
public:
    ~SocketSender();
    bool Start(int port = 6016);
    bool Send(const std::string& text);
    void Close();

    bool IsFailed() { return m_failed; }

private:
    int  m_sock{};
    bool m_failed{};
};
