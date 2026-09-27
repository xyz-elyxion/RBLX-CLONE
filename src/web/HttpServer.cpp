#include "HttpServer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <cstring>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <signal.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    constexpr socket_t kInvalidSocket = -1;
#endif

namespace web {

namespace {

constexpr size_t kMaxHeaderBytes = 32 * 1024;
constexpr size_t kMaxBodyBytes = 8 * 1024 * 1024;

void closeSocket(socket_t sock) {
#if defined(_WIN32)
    ::closesocket(sock);
#else
    ::close(sock);
#endif
}

bool sendAll(socket_t sock, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int chunk = static_cast<int>(std::min<size_t>(length - sent, 1 << 20));
        int result =
#if defined(_WIN32)
            ::send(sock, data + sent, chunk, 0);
#else
            static_cast<int>(::send(sock, data + sent, chunk, 0));
#endif
        if (result <= 0) return false;
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool sendAll(socket_t sock, const std::string& data) {
    return sendAll(sock, data.data(), data.size());
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string statusMessage(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segments;
    std::string current;
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) segments.push_back(current);
    return segments;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() && hexValue(value[i + 1]) >= 0 && hexValue(value[i + 2]) >= 0) {
            out += static_cast<char>((hexValue(value[i + 1]) << 4) | hexValue(value[i + 2]));
            i += 2;
        } else {
            out += value[i];
        }
    }
    return out;
}

void writeResponse(socket_t sock, const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " " << statusMessage(response.status) << "\r\n";
    bool hasConnection = false;
    for (const auto& header : response.headers) {
        out << header.first << ": " << header.second << "\r\n";
        if (toLower(header.first) == "connection") hasConnection = true;
    }
    if (!hasConnection) out << "Connection: close\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n\r\n";
    out << response.body;
    sendAll(sock, out.str());
}

void writeSimpleError(socket_t sock, int status, const char* message) {
    HttpResponse response;
    response.status = status;
    response.set("Content-Type", "application/json");
    response.body = std::string("{\"error\":\"") + message + "\"}";
    writeResponse(sock, response);
}

} // namespace

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::addRoute(const std::string& method, const std::string& pattern, PathHandler handler) {
    Route route;
    route.method = method;
    route.segments = splitPath(pattern);
    route.handler = handler;
    routes_.push_back(route);
}

bool HttpServer::listen(const std::string& host, int port, std::string& error) {
#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        error = "WSAStartup failed";
        return false;
    }
#else
    ::signal(SIGPIPE, SIG_IGN);
#endif

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* info = nullptr;
    const std::string portString = std::to_string(port);
    if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), portString.c_str(), &hints, &info) != 0 || !info) {
        error = "getaddrinfo failed for host: " + host;
        return false;
    }

    listenSocket_ = static_cast<uintptr_t>(::socket(info->ai_family, info->ai_socktype, info->ai_protocol));
    if (listenSocket_ == static_cast<uintptr_t>(kInvalidSocket)) {
        error = "socket() failed";
        ::freeaddrinfo(info);
        return false;
    }

    int reuse = 1;
    ::setsockopt(static_cast<socket_t>(listenSocket_), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (::bind(static_cast<socket_t>(listenSocket_), info->ai_addr, static_cast<int>(info->ai_addrlen)) != 0) {
        error = "bind() failed on port " + std::to_string(port);
        ::freeaddrinfo(info);
        return false;
    }
    ::freeaddrinfo(info);

    if (::listen(static_cast<socket_t>(listenSocket_), 64) != 0) {
        error = "listen() failed on port " + std::to_string(port);
        return false;
    }
    return true;
}

void HttpServer::run() {
    while (!stopRequested_) {
        socket_t client = ::accept(static_cast<socket_t>(listenSocket_), nullptr, nullptr);
        if (client == kInvalidSocket) {
            if (stopRequested_) break;
            continue;
        }
        std::thread(&HttpServer::serveConnection, this, static_cast<uintptr_t>(client)).detach();
    }
}

void HttpServer::stop() {
    stopRequested_ = true;
    if (listenSocket_ != 0) {
#if defined(_WIN32)
        ::closesocket(reinterpret_cast<socket_t>(listenSocket_));
        ::WSACleanup();
#else
        ::shutdown(static_cast<socket_t>(listenSocket_), SHUT_RDWR);
        ::close(static_cast<socket_t>(listenSocket_));
#endif
        listenSocket_ = 0;
    }
}

void HttpServer::serveConnection(uintptr_t rawSocket) {
    socket_t sock = static_cast<socket_t>(rawSocket);

    std::string buffer;
    buffer.reserve(16 * 1024);
    char chunk[16 * 1024];

    // Read until the end of the headers.
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) {
            writeSimpleError(sock, 413, "Request headers too large.");
            closeSocket(sock);
            return;
        }
        int received =
#if defined(_WIN32)
            ::recv(sock, chunk, sizeof(chunk), 0);
#else
            static_cast<int>(::recv(sock, chunk, sizeof(chunk), 0));
#endif
        if (received <= 0) break;
        buffer.append(chunk, static_cast<size_t>(received));
        headerEnd = buffer.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) {
        closeSocket(sock);
        return;
    }

    HttpRequest request;

    std::istringstream head(buffer.substr(0, headerEnd));
    std::string line;
    if (!std::getline(head, line)) {
        closeSocket(sock);
        return;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream requestLine(line);
        std::string target;
        requestLine >> request.method >> target;
        size_t queryStart = target.find('?');
        if (queryStart != std::string::npos) {
            request.query = target.substr(queryStart + 1);
            request.target = target.substr(0, queryStart);
        } else {
            request.target = target;
        }
        request.path = percentDecode(request.target);
    }

    size_t contentLength = 0;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = toLower(line.substr(0, colon));
        size_t valueStart = line.find_first_not_of(" \t", colon + 1);
        if (valueStart == std::string::npos) continue;
        request.headers[name] = line.substr(valueStart);
        if (name == "content-length") {
            contentLength = static_cast<size_t>(std::strtoull(line.c_str() + valueStart, nullptr, 10));
        }
    }

    if (contentLength > kMaxBodyBytes) {
        writeSimpleError(sock, 413, "Request body too large.");
        closeSocket(sock);
        return;
    }

    // Body: whatever arrived after the header terminator, then read the remainder.
    size_t bodyStart = headerEnd + 4;
    if (buffer.size() > bodyStart) {
        request.body = buffer.substr(bodyStart);
    }
    while (request.body.size() < contentLength) {
        int received =
#if defined(_WIN32)
            ::recv(sock, chunk, sizeof(chunk), 0);
#else
            static_cast<int>(::recv(sock, chunk, sizeof(chunk), 0));
#endif
        if (received <= 0) break;
        request.body.append(chunk, static_cast<size_t>(received));
    }
    if (request.body.size() > contentLength) {
        request.body.resize(contentLength);
    }

    HttpResponse response = dispatch(request);
    writeResponse(sock, response);
    closeSocket(sock);
}

HttpResponse HttpServer::dispatch(const HttpRequest& request) const {
    std::vector<std::string> segments = splitPath(request.path);
    std::vector<std::string> captures;

    for (const Route& route : routes_) {
        if (matchRoute(route, request.method, segments, captures)) {
            return route.handler(request, user_, captures);
        }
    }
    if (fallback_) {
        return fallback_(request, user_);
    }

    HttpResponse response;
    response.status = 404;
    response.set("Content-Type", "application/json");
    response.body = "{\"error\":\"Not found.\"}";
    return response;
}

bool HttpServer::matchRoute(const Route& route, const std::string& method,
                            const std::vector<std::string>& segments, std::vector<std::string>& captures) {
    if (route.method != method) return false;

    // A trailing "*" segment matches one or more remaining path segments.
    const bool wildcard = !route.segments.empty() && route.segments.back() == "*";
    if (wildcard) {
        if (segments.size() < route.segments.size()) return false;
    } else if (route.segments.size() != segments.size()) {
        return false;
    }

    captures.clear();
    const size_t literalCount = wildcard ? route.segments.size() - 1 : route.segments.size();
    for (size_t i = 0; i < literalCount; ++i) {
        const std::string& pattern = route.segments[i];
        if (!pattern.empty() && pattern[0] == ':') {
            captures.push_back(segments[i]);
        } else if (pattern != segments[i]) {
            return false;
        }
    }
    return true;
}

} // namespace web
