#pragma once
// SukiCode Socket 类型 - 网络套接字
// TCP/UDP socket abstraction.

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

namespace suki::network {

// 套接字地址 / Socket address
struct SocketAddress {
    std::string host;
    int port;

    std::string toString() const { return host + ":" + std::to_string(port); }
};

// TCP 套接字 / TCP Socket
class TCPSocket {
public:
    TCPSocket() : fd_(-1) {}
    ~TCPSocket() { close(); }

    // 禁止拷贝 / No copy
    TCPSocket(const TCPSocket&) = delete;
    TCPSocket& operator=(const TCPSocket&) = delete;

    // 允许移动 / Allow move
    TCPSocket(TCPSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    // 连接到服务器 / Connect to server
    bool connect(const std::string& host, int port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
            close();
            return false;
        }

        bool success = (::connect(fd_, result->ai_addr, result->ai_addrlen) == 0);
        freeaddrinfo(result);
        if (!success) close();
        return success;
    }

    // 绑定和监听 / Bind and listen
    bool bind(int port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        int opt = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        return (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    }

    bool listen(int backlog = 128) {
        return (::listen(fd_, backlog) == 0);
    }

    // 接受连接 / Accept connection
    TCPSocket accept() {
        TCPSocket client;
        struct sockaddr_in addr = {};
        socklen_t len = sizeof(addr);
        client.fd_ = ::accept(fd_, (struct sockaddr*)&addr, &len);
        return client;
    }

    // 发送数据 / Send data
    int send(const void* data, size_t len) {
        return ::send(fd_, (const char*)data, len, 0);
    }

    int send(const std::string& data) {
        return send(data.data(), data.size());
    }

    // 接收数据 / Receive data
    int receive(void* buffer, size_t len) {
        return recv(fd_, (char*)buffer, len, 0);
    }

    std::string receiveAll(size_t bufferSize = 4096) {
        std::string result;
        std::vector<char> buffer(bufferSize);
        int n;
        while ((n = receive(buffer.data(), bufferSize)) > 0) {
            result.append(buffer.data(), n);
        }
        return result;
    }

    // 关闭 / Close
    void close() {
        if (fd_ >= 0) {
#ifdef _WIN32
            closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
        }
    }

    bool isOpen() const { return fd_ >= 0; }

private:
    int fd_;
};

// UDP 套接字 / UDP Socket
class UDPSocket {
public:
    UDPSocket() : fd_(-1) {}
    ~UDPSocket() { close(); }

    bool open() {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        return fd_ >= 0;
    }

    bool bind(int port) {
        if (fd_ < 0) open();
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        return (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    }

    int sendTo(const void* data, size_t len, const std::string& host, int port) {
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        return sendto(fd_, (const char*)data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    }

    int receiveFrom(void* buffer, size_t len, std::string& fromHost, int& fromPort) {
        struct sockaddr_in addr = {};
        socklen_t addrLen = sizeof(addr);
        int n = recvfrom(fd_, (char*)buffer, len, 0, (struct sockaddr*)&addr, &addrLen);
        if (n > 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            fromHost = ip;
            fromPort = ntohs(addr.sin_port);
        }
        return n;
    }

    void close() {
        if (fd_ >= 0) {
#ifdef _WIN32
            closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_ = -1;
        }
    }

private:
    int fd_;
};

// 网络初始化 / Network initialization
inline void initializeNetwork() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

inline void cleanupNetwork() {
#ifdef _WIN32
    WSACleanup();
#endif
}

} // namespace suki::network
