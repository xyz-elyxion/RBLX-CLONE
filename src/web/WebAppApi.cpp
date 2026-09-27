#include "WebApp.h"
#include "Db.h"
#include "WebCrypto.h"
#include "WebUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unordered_map>

namespace web {

// ---------------------------------------------------------------------------
// Auth helpers
// ---------------------------------------------------------------------------

std::string WebApp::bearerToken(const HttpRequest& request) const {
    const auto it = request.headers.find("authorization");
    if (it == request.headers.end()) return "";
    if (it->second.rfind("Bearer ", 0) == 0) return it->second.substr(7);
    if (it->second.rfind("bearer ", 0) == 0) return it->second.substr(7);
    return it->second;
}

bool WebApp::authenticate(const HttpRequest& request, int64_t& userId, std::string& username,
                          HttpResponse& errorResponseOut) const {
    const std::string token = bearerToken(request);
    if (token.empty()) {
        errorResponseOut = util::errorResponse(401, "Authorization bearer token is required.");
        return false;
    }

    int64_t decodedUserId = 0;
    std::string decodedUsername;
    if (jwtVerify(token, config_.jwtSecret, decodedUserId, decodedUsername) != 0) {
        errorResponseOut = util::errorResponse(401, "Invalid or expired token.");
        return false;
    }

    Statement stmt(db_, "SELECT id, username FROM users WHERE id = ?");
    stmt.bind(1, decodedUserId);
    if (!stmt.ok() || !stmt.step()) {
        errorResponseOut = util::errorResponse(401, "Invalid or expired token.");
        return false;
    }

    userId = stmt.getInt(0);
    username = stmt.getText(1);
    return true;
}

bool WebApp::optionalAuthenticate(const HttpRequest& request, int64_t& userId, std::string& username) const {
    int64_t decodedUserId = 0;
    std::string decodedUsername;
    if (jwtVerify(bearerToken(request), config_.jwtSecret, decodedUserId, decodedUsername) == 0) {
        Statement stmt(db_, "SELECT id, username FROM users WHERE id = ?");
        stmt.bind(1, decodedUserId);
        if (stmt.ok() && stmt.step()) {
            userId = stmt.getInt(0);
            username = stmt.getText(1);
            return true;
        }
    }
    return false;
}

bool WebApp::issueToken(int64_t userId, const std::string& username, std::string& token) const {
    token = jwtSign(config_.jwtSecret, userId, username, config_.jwtExpiresInSeconds);
    return !token.empty();
}

// ---------------------------------------------------------------------------
// JSON body handling
// ---------------------------------------------------------------------------

bool WebApp::requireJsonContent(const HttpRequest& request, HttpResponse& errorResponseOut) const {
    const auto it = request.headers.find("content-type");
    if (it == request.headers.end() || it->second.find("application/json") == std::string::npos) {
        errorResponseOut = util::errorResponse(415, "Content-Type must be application/json.");
        return false;
    }
    return true;
}

bool WebApp::jsonBody(const HttpRequest& request, json& body, HttpResponse& errorResponseOut) const {
    if (!requireJsonContent(request, errorResponseOut)) return false;

    if (request.body.empty()) {
        body = json::object();
        return true;
    }
    try {
        body = json::parse(request.body);
    } catch (const json::parse_error&) {
        errorResponseOut = util::errorResponse(400, "Malformed JSON body.");
        return false;
    }
    if (!body.is_object()) {
        errorResponseOut = util::errorResponse(400, "Malformed JSON body.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Static files + fallback routing
// ---------------------------------------------------------------------------

HttpResponse WebApp::handleFallback(const HttpRequest& request, void* user) {
    auto* app = static_cast<WebApp*>(user);
    return app->onFallback(request);
}

HttpResponse WebApp::onFallback(const HttpRequest& request) {
    const std::string& path = request.path;

    if (request.method == "GET" || request.method == "HEAD") {
        if (path == "/") return serveStaticFile("/index.html");
        if (path == "/login") return serveStaticFile("/login.html");
        if (path == "/signup") return serveStaticFile("/signup.html");
        if (path == "/dashboard") return serveStaticFile("/dashboard.html");
        if (path == "/avatar") return serveStaticFile("/avatar.html");
        if (path == "/search") return serveStaticFile("/search.html");
        if (path == "/games") return serveStaticFile("/games.html");
        if (path == "/create") return serveStaticFile("/create.html");
        if (path.rfind("/games/", 0) == 0) return serveStaticFile("/game.html");
        if (path == "/profile" || path.rfind("/profile/", 0) == 0) return serveStaticFile("/profile.html");
    }

    return serveStaticFile(path);
}

HttpResponse WebApp::serveStaticFile(const std::string& urlPath) {
    std::string relative = urlPath;
    if (relative == "/" || relative.empty()) relative = "/index.html";

    if (relative.find("..") != std::string::npos || relative.find('\\') != std::string::npos) {
        return util::errorResponse(400, "Bad request.");
    }

    // Strip the leading slash so filesystem::path joins onto publicDir instead
    // of replacing it (POSIX treats "/x" as absolute).
    if (!relative.empty() && relative.front() == '/') relative.erase(0, 1);

    const std::string fullPath = util::joinPath(config_.publicDir, relative);
    if (!util::fileExists(fullPath)) return util::notFound();

    std::string content;
    if (!util::readFile(fullPath, content)) return util::notFound();

    HttpResponse response;
    response.status = 200;
    response.set("Content-Type", contentTypeFor(fullPath));
    response.body = std::move(content);
    return response;
}

std::string WebApp::contentTypeFor(const std::string& path) const {
    static const std::unordered_map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"},
        {".json", "application/json; charset=utf-8"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
        {".svg", "image/svg+xml"}, {".ico", "image/x-icon"}, {".txt", "text/plain; charset=utf-8"},
        {".glsl", "text/plain; charset=utf-8"}, {".world", "text/plain; charset=utf-8"}
    };

    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const auto it = types.find(util::lower(path.substr(dot)));
    return it != types.end() ? it->second : "application/octet-stream";
}

// ---------------------------------------------------------------------------
// API dispatch
// ---------------------------------------------------------------------------

HttpResponse WebApp::handleApi(const HttpRequest& request, void* user, const std::vector<std::string>& captures) {
    auto* app = static_cast<WebApp*>(user);
    return app->onApi(request, captures);
}

HttpResponse WebApp::onApi(const HttpRequest& request, const std::vector<std::string>& captures) {
    const std::string& method = request.method;
    // Full path after "/api/".
    const std::string sub = request.path.size() > 5 ? request.path.substr(5) : "";

    if (method == "OPTIONS") {
        HttpResponse response;
        response.status = 204;
        response.set("Access-Control-Allow-Origin", "*");
        response.set("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
        response.set("Access-Control-Allow-Headers", "Content-Type, Authorization");
        return response;
    }

    // Simple exact routes.
    if (method == "POST" && sub == "signup") return signup(request);
    if (method == "POST" && sub == "login") return login(request);
    if (method == "POST" && sub == "verify") return verify(request);
    if (method == "GET" && sub == "games") return gamesList(request);
    if (method == "GET" && sub == "avatar") {
        int64_t userId = 0;
        std::string username;
        HttpResponse error;
        if (!authenticate(request, userId, username, error)) return error;
        return avatarGet(request, userId);
    }
    if (method == "POST" && sub == "avatar") {
        int64_t userId = 0;
        std::string username;
        HttpResponse error;
        if (!authenticate(request, userId, username, error)) return error;
        return avatarPost(request, userId);
    }

    // Authenticated "me" routes.
    if (sub == "me/social" || sub == "me/playtime") {
        int64_t userId = 0;
        std::string username;
        HttpResponse error;
        if (!authenticate(request, userId, username, error)) return error;
        if (method == "GET" && sub == "me/social") return meSocial(request, userId);
        if (method == "POST" && sub == "me/playtime") return mePlaytime(request, userId);
        return util::notFound();
    }

    // Heartbeats: /api/game-instances/:id/heartbeat
    if (method == "POST" && sub.rfind("game-instances/", 0) == 0) {
        const size_t first = sub.find('/');
        const size_t second = sub.find('/', first + 1);
        if (second != std::string::npos && sub.compare(second, 11, "/heartbeat") == 0 && second > first + 1) {
            return heartbeat(request, sub.substr(first + 1, second - first - 1));
        }
        return util::notFound();
    }

    // Users.
    if (sub.rfind("users/", 0) == 0 && (method == "GET")) {
        const std::string rest = sub.substr(6);
        int64_t userId = 0;
        std::string username;
        HttpResponse error;
        if (!authenticate(request, userId, username, error)) return error;
        if (rest == "search") return usersSearch(request, userId);
        return userDetail(request, userId, rest);
    }

    // Friends.
    if (method == "POST" && (sub == "friends/request" || sub == "friends/respond" || sub == "friends/remove")) {
        int64_t userId = 0;
        std::string username;
        HttpResponse error;
        if (!authenticate(request, userId, username, error)) return error;
        if (sub == "friends/request") return friendsRequest(request, userId);
        if (sub == "friends/respond") return friendsRespond(request, userId);
        return friendsRemove(request, userId);
    }

    // Games.
    if (method == "POST" && sub == "games") {
        int64_t userId = 0;
        std::string username;
        HttpResponse error;
        if (!authenticate(request, userId, username, error)) return error;
        return gamesCreate(request, userId);
    }
    if (sub.rfind("games/", 0) == 0) {
        const std::string rest = sub.substr(6);

        if (method == "GET" && rest == "mine") {
            int64_t userId = 0;
            std::string username;
            HttpResponse error;
            if (!authenticate(request, userId, username, error)) return error;
            return gamesMine(request, userId);
        }
        if (method == "POST" && rest == "publish") {
            int64_t userId = 0;
            std::string username;
            HttpResponse error;
            if (!authenticate(request, userId, username, error)) return error;
            return gamesPublish(request, userId);
        }

        // games/:id[/servers|/play]
        const size_t slash = rest.find('/');
        const std::string gameIdText = slash == std::string::npos ? rest : rest.substr(0, slash);
        const std::string tail = slash == std::string::npos ? "" : rest.substr(slash + 1);

        if (method == "GET" && tail.empty()) {
            int64_t viewerId = 0;
            std::string username;
            optionalAuthenticate(request, viewerId, username);
            return gameDetail(request, viewerId, gameIdText);
        }
        if (method == "GET" && tail == "servers") {
            int64_t viewerId = 0;
            std::string username;
            optionalAuthenticate(request, viewerId, username);
            return gameServers(request, viewerId, gameIdText);
        }
        if (method == "PUT" && tail.empty()) {
            int64_t userId = 0;
            std::string username;
            HttpResponse error;
            if (!authenticate(request, userId, username, error)) return error;
            return gameUpdate(request, userId, gameIdText);
        }
        if (method == "POST" && tail == "play") {
            int64_t userId = 0;
            std::string username;
            HttpResponse error;
            if (!authenticate(request, userId, username, error)) return error;
            return gamePlay(request, userId, gameIdText);
        }
    }

    (void)captures;
    return util::notFound();
}

// ---------------------------------------------------------------------------
// Auth endpoints
// ---------------------------------------------------------------------------

HttpResponse WebApp::signup(const HttpRequest& request) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    const std::string username = body.value("username", std::string());
    const std::string password = body.value("password", std::string());

    const bool usernameOk = username.size() >= 3 && username.size() <= 20 &&
                            std::all_of(username.begin(), username.end(), [](char c) {
                                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                       (c >= '0' && c <= '9') || c == '_';
                            });
    if (!usernameOk) {
        return util::errorResponse(400,
                                   "Username must be 3-20 characters and use only letters, numbers, or underscores.");
    }
    if (password.size() < 8 || password.size() > 128) {
        return util::errorResponse(400, "Password must be between 8 and 128 characters.");
    }

    const std::string hash = bcryptHash(password, config_.bcryptRounds);
    if (hash.empty()) return util::errorResponse(500, "Server error.");

    {
        Statement stmt(db_, "INSERT INTO users (username, password_hash) VALUES (?, ?)");
        stmt.bind(1, username);
        stmt.bind(2, hash);
        if (!stmt.ok() || !stmt.exec()) {
            return util::errorResponse(409, "Username already exists.");
        }
    }

    const int64_t userId = lastInsertRowId(db_);
    std::string token;
    if (!issueToken(userId, username, token)) return util::errorResponse(500, "Server error.");

    return util::jsonResponse(201,
                              json{{"success", true}, {"token", token}, {"userId", userId}, {"username", username}});
}

HttpResponse WebApp::login(const HttpRequest& request) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    const std::string username = body.value("username", std::string());
    const std::string password = body.value("password", std::string());

    if (username.empty() || password.empty() || username.size() > 20 || password.size() > 128) {
        return util::errorResponse(400, "Invalid username or password.");
    }

    Statement stmt(db_, "SELECT id, username, password_hash FROM users WHERE username = ?");
    stmt.bind(1, username);
    if (!stmt.ok() || !stmt.step()) {
        return util::errorResponse(401, "Invalid username or password.");
    }
    const int64_t userId = stmt.getInt(0);
    const std::string storedUsername = stmt.getText(1);
    const std::string passwordHash = stmt.getText(2);

    if (!bcryptVerify(password, passwordHash)) {
        return util::errorResponse(401, "Invalid username or password.");
    }

    std::string token;
    if (!issueToken(userId, storedUsername, token)) return util::errorResponse(500, "Server error.");
    return util::jsonResponse(
        200, json{{"success", true}, {"token", token}, {"userId", userId}, {"username", storedUsername}});
}

HttpResponse WebApp::verify(const HttpRequest& request) {
    std::string token = bearerToken(request);
    bool deprecated = false;

    if (token.empty()) {
        json body;
        HttpResponse error;
        if (!jsonBody(request, body, error)) return error;
        if (body.contains("token") && body["token"].is_string()) {
            deprecated = true;
            token = body["token"].get<std::string>();
        }
    }
    if (token.empty()) {
        return util::errorResponse(401, "Authorization bearer token is required.");
    }

    int64_t userId = 0;
    std::string username;
    if (jwtVerify(token, config_.jwtSecret, userId, username) != 0) {
        return util::errorResponse(401, "Invalid or expired token.");
    }

    Statement stmt(db_, "SELECT id, username FROM users WHERE id = ?");
    stmt.bind(1, userId);
    if (!stmt.ok() || !stmt.step()) {
        return util::errorResponse(401, "Invalid or expired token.");
    }

    HttpResponse response = util::jsonResponse(
        200, json{{"success", true}, {"userId", stmt.getInt(0)}, {"username", stmt.getText(1)}});
    if (deprecated) response.set("Deprecation", "true");
    return response;
}

} // namespace web
