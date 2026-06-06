#pragma once
// SukiCode URL 类型 - URL 解析和操作
// URL parsing and manipulation.

#include <string>
#include <sstream>

namespace suki::stdlib {

class URL {
public:
    URL() = default;
    URL(const std::string& urlString) : urlString_(urlString) { parse(); }

    // 组件 / Components
    std::string scheme() const { return scheme_; }
    std::string host() const { return host_; }
    int port() const { return port_; }
    std::string path() const { return path_; }
    std::string query() const { return query_; }
    std::string fragment() const { return fragment_; }
    std::string string() const { return urlString_; }

    // 构建 / Build
    static URL fromComponents(const std::string& scheme, const std::string& host,
                               int port = 0, const std::string& path = "/") {
        URL url;
        url.scheme_ = scheme;
        url.host_ = host;
        url.port_ = port;
        url.path_ = path;
        url.urlString_ = scheme + "://" + host;
        if (port > 0) url.urlString_ += ":" + std::to_string(port);
        url.urlString_ += path;
        return url;
    }

    // 修改 / Modification
    URL appending(const std::string& component) const {
        URL result = *this;
        if (!result.path_.empty() && result.path_.back() != '/') {
            result.path_ += '/';
        }
        result.path_ += component;
        result.urlString_ = result.scheme_ + "://" + result.host_;
        if (result.port_ > 0) result.urlString_ += ":" + std::to_string(result.port_);
        result.urlString_ += result.path_;
        return result;
    }

    // 检查 / Checks
    bool isValid() const { return !scheme_.empty() && !host_.empty(); }
    bool isSecure() const { return scheme_ == "https" || scheme_ == "wss"; }

private:
    void parse() {
        // 简单的 URL 解析 / Simple URL parsing
        size_t pos = 0;

        // scheme
        size_t schemeEnd = urlString_.find("://");
        if (schemeEnd != std::string::npos) {
            scheme_ = urlString_.substr(0, schemeEnd);
            pos = schemeEnd + 3;
        }

        // host:port
        size_t hostEnd = urlString_.find_first_of("/?#", pos);
        std::string hostPort = urlString_.substr(pos, hostEnd - pos);
        size_t colonPos = hostPort.find(':');
        if (colonPos != std::string::npos) {
            host_ = hostPort.substr(0, colonPos);
            port_ = std::stoi(hostPort.substr(colonPos + 1));
        } else {
            host_ = hostPort;
        }
        pos = hostEnd;

        // path
        if (pos < urlString_.size() && urlString_[pos] == '/') {
            size_t pathEnd = urlString_.find_first_of("?#", pos);
            path_ = urlString_.substr(pos, pathEnd - pos);
            pos = pathEnd;
        }

        // query
        if (pos < urlString_.size() && urlString_[pos] == '?') {
            size_t queryEnd = urlString_.find('#', pos);
            query_ = urlString_.substr(pos + 1, queryEnd - pos - 1);
            pos = queryEnd;
        }

        // fragment
        if (pos < urlString_.size() && urlString_[pos] == '#') {
            fragment_ = urlString_.substr(pos + 1);
        }
    }

    std::string urlString_;
    std::string scheme_;
    std::string host_;
    int port_ = 0;
    std::string path_ = "/";
    std::string query_;
    std::string fragment_;
};

} // namespace suki::stdlib
