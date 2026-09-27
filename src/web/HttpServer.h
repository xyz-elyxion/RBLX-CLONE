#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace web {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string target;                       // raw request target, e.g. /api/games?q=x
    std::string query;                        // raw query string without '?'
    std::map<std::string, std::string> headers; // keys lower-cased
    std::string body;
    std::string remoteAddress;
};

struct HttpResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    void set(const std::string& name, const std::string& value) {
        headers.emplace_back(name, value);
    }
};

// Blocking, thread-per-connection HTTP/1.1 server used by the Limey web backend.
// Uses WinSock on Windows and POSIX sockets elsewhere.
class HttpServer {
public:
    using Handler = HttpResponse (*)(const HttpRequest&, void* user);
    using PathHandler = HttpResponse (*)(const HttpRequest&, void* user, const std::vector<std::string>& captures);

    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Route table: literal segments must match exactly; ":name" segments become captures.
    void addRoute(const std::string& method, const std::string& pattern, PathHandler handler);

    // Fallback for routes that need custom matching (static files, HEAD handling).
    void setFallback(Handler handler) { fallback_ = handler; }

    // Opaque context passed to every handler (the WebApp instance).
    void setUser(void* user) { user_ = user; }

    // Binds and listens. Returns false with message filled in on failure.
    bool listen(const std::string& host, int port, std::string& error);
    void run();          // accept loop, blocks until stopRequested
    void stop();

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        PathHandler handler;
    };

    void serveConnection(uintptr_t socket);
    HttpResponse dispatch(const HttpRequest& request) const;
    static bool matchRoute(const Route& route, const std::string& method,
                           const std::vector<std::string>& segments, std::vector<std::string>& captures);

    std::vector<Route> routes_;
    Handler fallback_ = nullptr;
    void* user_ = nullptr;
    uintptr_t listenSocket_ = 0;
    bool stopRequested_ = false;
};

} // namespace web
