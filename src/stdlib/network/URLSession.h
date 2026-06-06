#pragma once
// SukiCode URLSession - HTTP 客户端
// HTTP client for making HTTP requests.

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace suki::network {

// HTTP 响应 / HTTP Response
struct HTTPResponse {
    int statusCode = 0;
    std::string statusMessage;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::vector<uint8_t> rawData;

    bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
    bool isRedirect() const { return statusCode >= 300 && statusCode < 400; }
    bool isClientError() const { return statusCode >= 400 && statusCode < 500; }
    bool isServerError() const { return statusCode >= 500; }
};

// HTTP 请求 / HTTP Request
struct HTTPRequest {
    std::string method = "GET";
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    int timeoutSeconds = 30;
};

// URLSession - HTTP 客户端 / HTTP Client
class URLSession {
public:
    // GET 请求 / GET request
    static HTTPResponse get(const std::string& url,
                            const std::unordered_map<std::string, std::string>& headers = {}) {
        HTTPRequest req;
        req.method = "GET";
        req.url = url;
        req.headers = headers;
        return perform(req);
    }

    // POST 请求 / POST request
    static HTTPResponse post(const std::string& url, const std::string& body,
                             const std::string& contentType = "application/json") {
        HTTPRequest req;
        req.method = "POST";
        req.url = url;
        req.body = body;
        req.headers["Content-Type"] = contentType;
        req.headers["Content-Length"] = std::to_string(body.size());
        return perform(req);
    }

    // PUT 请求 / PUT request
    static HTTPResponse put(const std::string& url, const std::string& body,
                            const std::string& contentType = "application/json") {
        HTTPRequest req;
        req.method = "PUT";
        req.url = url;
        req.body = body;
        req.headers["Content-Type"] = contentType;
        req.headers["Content-Length"] = std::to_string(body.size());
        return perform(req);
    }

    // DELETE 请求 / DELETE request
    static HTTPResponse del(const std::string& url) {
        HTTPRequest req;
        req.method = "DELETE";
        req.url = url;
        return perform(req);
    }

    // 执行请求 / Perform request
    static HTTPResponse perform(const HTTPRequest& request) {
        HTTPResponse response;

        // 解析 URL / Parse URL
        std::string host, path;
        int port = 80;
        parseURL(request.url, host, port, path);

        // 建立 TCP 连接 / Establish TCP connection
        int sock = createConnection(host, port);
        if (sock < 0) {
            response.statusCode = -1;
            response.statusMessage = "Connection failed";
            return response;
        }

        // 构建 HTTP 请求 / Build HTTP request
        std::ostringstream reqStream;
        reqStream << request.method << " " << path << " HTTP/1.1\r\n";
        reqStream << "Host: " << host << "\r\n";
        reqStream << "Connection: close\r\n";
        for (const auto& [key, value] : request.headers) {
            reqStream << key << ": " << value << "\r\n";
        }
        reqStream << "\r\n";
        if (!request.body.empty()) {
            reqStream << request.body;
        }

        // 发送请求 / Send request
        std::string reqStr = reqStream.str();
        send(sock, reqStr.c_str(), reqStr.size(), 0);

        // 接收响应 / Receive response
        std::string responseStr;
        char buffer[4096];
        int bytesRead;
        while ((bytesRead = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
            responseStr.append(buffer, bytesRead);
        }

        closeSocket(sock);

        // 解析响应 / Parse response
        parseResponse(responseStr, response);

        return response;
    }

private:
    static void parseURL(const std::string& url, std::string& host, int& port, std::string& path) {
        size_t pos = 0;
        std::string scheme = "http";

        // 跳过方案 / Skip scheme
        if (url.substr(0, 7) == "http://") {
            pos = 7;
            scheme = "http";
        } else if (url.substr(0, 8) == "https://") {
            pos = 8;
            scheme = "https";
            port = 443;
        }

        // 查找路径 / Find path
        size_t pathStart = url.find('/', pos);
        std::string hostPort;
        if (pathStart != std::string::npos) {
            hostPort = url.substr(pos, pathStart - pos);
            path = url.substr(pathStart);
        } else {
            hostPort = url.substr(pos);
            path = "/";
        }

        // 解析主机和端口 / Parse host and port
        size_t colonPos = hostPort.find(':');
        if (colonPos != std::string::npos) {
            host = hostPort.substr(0, colonPos);
            port = std::stoi(hostPort.substr(colonPos + 1));
        } else {
            host = hostPort;
        }
    }

    static int createConnection(const std::string& host, int port) {
        struct addrinfo hints = {}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        std::string portStr = std::to_string(port);
        if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
            return -1;
        }

        int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock < 0) {
            freeaddrinfo(result);
            return -1;
        }

        if (connect(sock, result->ai_addr, result->ai_addrlen) < 0) {
            closeSocket(sock);
            freeaddrinfo(result);
            return -1;
        }

        freeaddrinfo(result);
        return sock;
    }

    static void closeSocket(int sock) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

    static void parseResponse(const std::string& raw, HTTPResponse& response) {
        size_t headerEnd = raw.find("\r\n\r\n");
        std::string headerPart = (headerEnd != std::string::npos) ? raw.substr(0, headerEnd) : raw;
        response.body = (headerEnd != std::string::npos) ? raw.substr(headerEnd + 4) : "";

        // 解析状态行 / Parse status line
        size_t firstLine = headerPart.find("\r\n");
        std::string statusLine = headerPart.substr(0, firstLine);

        // HTTP/1.1 200 OK
        size_t space1 = statusLine.find(' ');
        size_t space2 = statusLine.find(' ', space1 + 1);
        if (space1 != std::string::npos && space2 != std::string::npos) {
            response.statusCode = std::stoi(statusLine.substr(space1 + 1, space2 - space1 - 1));
            response.statusMessage = statusLine.substr(space2 + 1);
        }

        // 解析头部 / Parse headers
        size_t pos = firstLine + 2;
        while (pos < headerPart.size()) {
            size_t lineEnd = headerPart.find("\r\n", pos);
            if (lineEnd == std::string::npos) lineEnd = headerPart.size();
            std::string line = headerPart.substr(pos, lineEnd - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                // 去除前导空格
                while (!value.empty() && value[0] == ' ') value.erase(0, 1);
                response.headers[key] = value;
            }
            pos = lineEnd + 2;
        }

        response.rawData.assign(response.body.begin(), response.body.end());
    }
};

} // namespace suki::network
