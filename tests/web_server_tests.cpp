#include "HttpServer.h"
#include "WebApp.h"
#include "WebCrypto.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

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
    using address_t = IN_ADDR;
    constexpr socket_t kInvalidSocketValue = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <cstring>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    using address_t = in_addr;
    constexpr socket_t kInvalidSocketValue = -1;
#endif

using json = nlohmann::json;

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) {
        std::cout << "ok   - " << label << std::endl;
    } else {
        ++failures;
        std::cout << "FAIL - " << label << std::endl;
    }
}

std::vector<std::pair<std::string, std::string>> parseSetCookies(const std::string& raw) {
    (void)raw;
    return {};
}

// --- tiny HTTP client for the integration tests -----------------------------

struct RawResponse {
    int status = 0;
    std::string headers;
    std::string body;
};

RawResponse httpRequest(int port, const std::string& method, const std::string& path,
                        const std::string& body = "", const std::string& token = "") {
    socket_t sock =
        ::socket(AF_INET, SOCK_STREAM,
#if defined(_WIN32)
                 IPPROTO_TCP
#else
                 0
#endif
        );
    RawResponse response;
    if (sock == kInvalidSocketValue) return response;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
#if defined(_WIN32)
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        return response;
    }

    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: localhost\r\n";
    request += "Content-Type: application/json\r\n";
    if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;

    size_t sent = 0;
    while (sent < request.size()) {
        const int chunk =
#if defined(_WIN32)
            ::send(sock, request.data() + sent, static_cast<int>(request.size() - sent), 0);
#else
            static_cast<int>(::send(sock, request.data() + sent, request.size() - sent, 0));
#endif
        if (chunk <= 0) break;
        sent += static_cast<size_t>(chunk);
    }

    std::string raw;
    char buffer[8192];
    for (;;) {
        const int received =
#if defined(_WIN32)
            ::recv(sock, buffer, sizeof(buffer), 0);
#else
            static_cast<int>(::recv(sock, buffer, sizeof(buffer), 0));
#endif
        if (received <= 0) break;
        raw.append(buffer, static_cast<size_t>(received));
    }
#if defined(_WIN32)
    ::closesocket(sock);
#else
    ::close(sock);
#endif

    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return response;
    response.headers = raw.substr(0, headerEnd);
    response.body = raw.substr(headerEnd + 4);
    if (response.headers.rfind("HTTP/1.1 ", 0) == 0) {
        response.status = std::atoi(response.headers.c_str() + 9);
    }
    return response;
}

#if !defined(_WIN32)
int _stricmp(const char* a, const char* b) {
    while (*a && *b) {
        const int ca = std::tolower(static_cast<unsigned char>(*a));
        const int cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb) return ca - cb;
        ++a; ++b;
    }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}
#endif

std::string headerValue(const RawResponse& response, const std::string& name) {
    std::istringstream stream(response.headers);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (_stricmp(name.c_str(), line.substr(0, colon).c_str()) == 0) {
            size_t start = line.find_first_not_of(" \t", colon + 1);
            return start == std::string::npos ? "" : line.substr(start);
        }
    }
    return "";
}

} // namespace

int main() {
    // ---- crypto unit checks ----
    check(web::sha256Hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "sha256 empty input digest");
    check(web::sha256Hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 \"abc\" digest");
    check(web::base64UrlEncode("\x01\x02\x03") == "AQID", "base64url encode");
    check(web::base64UrlDecode("AQID") == std::string("\x01\x02\x03", 3), "base64url decode");
    check(web::parseJwtExpiresInSeconds("7d", 0) == 7 * 24 * 3600, "JWT expiry parsing (7d)");
    check(web::parseJwtExpiresInSeconds("30m", 0) == 1800, "JWT expiry parsing (30m)");

    {
        const std::string token = web::jwtSign("test-secret", 42, "Player_1", 3600);
        int64_t userId = 0;
        std::string username;
        check(web::jwtVerify(token, "test-secret", userId, username) == 0, "jwt verify round-trip");
        check(userId == 42 && username == "Player_1", "jwt claims round-trip");
        check(web::jwtVerify(token + "x", "test-secret", userId, username) != 0, "jwt tamper rejected");
        check(web::jwtVerify(token, "wrong-secret", userId, username) != 0, "jwt wrong secret rejected");
    }

    {
        const std::string hash = web::bcryptHash("long-password", 4);
        check(hash.rfind("$2", 0) == 0, "bcrypt hash format");
        check(web::bcryptVerify("long-password", hash), "bcrypt verify accepts correct password");
        check(!web::bcryptVerify("wrong-password", hash), "bcrypt verify rejects wrong password");
    }

    // ---- integration: boot the real server on an ephemeral port ----
    web::WebApp::Config config = web::WebApp::Config{};
    config.nodeEnv = "test";
    config.port = 0; // resolved below via a fixed high port fallback
    config.databasePath = ":memory:";
    config.jwtSecret = "test-secret-value-at-least-32-chars!!";
    config.bcryptRounds = 4;
    config.repoRoot = ".";
    config.publicDir = "web/public";
    config.gameWorldsDir = "web/test-game-worlds";
    config.jwtExpiresInSeconds = 3600;
    // Use a fixed test port to keep the harness simple across platforms.
    config.port = 39701;

    web::WebApp app;
    std::string error;
    if (!app.initialize(config, error)) {
        std::cout << "FAIL - server initialize: " << error << std::endl;
        return 1;
    }
    if (!app.listen(error)) {
        std::cout << "FAIL - server listen: " << error << std::endl;
        return 1;
    }
    std::thread serverThread([&app]() { app.run(); });

    const int port = config.port;

    // Static site
    {
        const RawResponse response = httpRequest(port, "GET", "/");
        check(response.status == 200, "GET / serves index");
        check(response.body.find("Limey") != std::string::npos, "index contains Limey brand");
    }

    // Signup + login + verify
    std::string token;
    {
        const RawResponse response =
            httpRequest(port, "POST", "/api/signup", R"({"username":"Player_1","password":"long-password"})");
        check(response.status == 201, "signup returns 201");
        const json parsed = json::parse(response.body);
        check(parsed.value("success", false) == true, "signup success flag");
        check(parsed.value("userId", 0) > 0, "signup userId");
        token = parsed.value("token", "");
        check(!token.empty(), "signup returns token");
    }
    {
        const RawResponse response =
            httpRequest(port, "POST", "/api/signup", R"({"username":"Player_1","password":"long-password"})");
        check(response.status == 409, "duplicate signup returns 409");
    }
    {
        const RawResponse response =
            httpRequest(port, "POST", "/api/login", R"({"username":"Player_1","password":"long-password"})");
        check(response.status == 200, "login returns 200");
        const json parsed = json::parse(response.body);
        check(parsed.value("success", false) == true, "login success flag");
    }
    {
        const RawResponse response =
            httpRequest(port, "POST", "/api/login", R"({"username":"Player_1","password":"wrong-password"})");
        check(response.status == 401, "bad login returns 401");
    }
    {
        const RawResponse response = httpRequest(port, "POST", "/api/verify", "{}", token);
        check(response.status == 200, "verify with bearer token");
    }
    {
        RawResponse response = httpRequest(port, "POST", "/api/verify", R"({"token":")" + token + "\"}");
        check(response.status == 200, "verify with deprecated body token");
        check(headerValue(response, "Deprecation") == "true", "deprecation header present");
    }

    // Avatar round-trip
    {
        const RawResponse response = httpRequest(port, "GET", "/api/avatar", "", token);
        check(response.status == 200, "avatar GET");
        const json parsed = json::parse(response.body);
        check(parsed["avatar"]["faceId"] == "classic", "default faceId is classic");
    }
    {
        const RawResponse response = httpRequest(port, "POST", "/api/avatar",
                                                 R"({"avatar":{"headColor":[1,0.5,0],"torsoColor":[0,0,1],)"
                                                 R"("leftArmColor":[0.8,0.6,0.4],"rightArmColor":[0.8,0.6,0.4],)"
                                                 R"("leftLegColor":[0.2,0.6,0.2],"rightLegColor":[0.2,0.6,0.2],)"
                                                 R"("faceId":"wink"}})",
                                                 token);
        check(response.status == 200, "avatar POST accepted");
        const RawResponse again = httpRequest(port, "GET", "/api/avatar", "", token);
        const json parsed = json::parse(again.body);
        check(parsed["avatar"]["faceId"] == "wink", "avatar persisted");
        check(parsed["avatar"]["headColor"][0] == 1.0, "avatar colors persisted");
    }

    // Social endpoints
    {
        const RawResponse response = httpRequest(port, "GET", "/api/me/social", "", token);
        check(response.status == 200, "me/social");
        const json parsed = json::parse(response.body);
        check(parsed.value("success", false), "me/social success flag");
        check(parsed["profile"]["relationship"] == "self", "self relationship");
    }

    // Games: seed + publish + detail
    {
        const RawResponse response = httpRequest(port, "GET", "/api/games");
        check(response.status == 200, "games list");
        const json parsed = json::parse(response.body);
        check(parsed["games"].is_array() && parsed["games"].size() >= 1, "starter games seeded");
    }
    std::string gameIdText;
    {
        const std::string worldText = "1\n0 \"Baseplate\" -1 0 0 0 0 0 0 10 1 10 0.3 0.5 0.3 0 0 0 0 0 1 1 0\n";
        json body;
        body["title"] = "CI Test World";
        body["description"] = "from tests";
        body["isPublic"] = true;
        body["worldText"] = worldText;
        const RawResponse response =
            httpRequest(port, "POST", "/api/games", body.dump(), token);
        check(response.status == 201, "game create returns 201");
        const json parsed = json::parse(response.body);
        check(parsed.value("success", false), "game create success flag");
        check(parsed["game"]["stats"]["partsCount"] == 1, "parts count analyzed");
        gameIdText = std::to_string(parsed["game"]["id"].get<int64_t>());
    }
    {
        const RawResponse response = httpRequest(port, "GET", "/api/games/" + gameIdText);
        check(response.status == 200, "game detail");
        const json parsed = json::parse(response.body);
        check(parsed["game"]["title"] == "CI Test World", "game detail title");
    }
    {
        const RawResponse response = httpRequest(port, "PUT", "/api/games/" + gameIdText,
                                                 R"({"title":"CI Test World 2"})", token);
        check(response.status == 200, "game update");
        const json parsed = json::parse(response.body);
        check(parsed["game"]["title"] == "CI Test World 2", "game update title");
    }
    {
        const RawResponse response = httpRequest(port, "POST", "/api/games/" + gameIdText + "/play",
                                                 R"({"launchClient":false})", token);
        check(response.status == 200, "game play returns launch metadata");
        const json parsed = json::parse(response.body);
        check(parsed["server"]["port"] > 0, "play assigns a port");
        check(parsed["launch"]["command"].get<std::string>().find("Client.exe") != std::string::npos,
              "play returns client command");
    }
    {
        const RawResponse response = httpRequest(port, "POST", "/api/games/999999/play",
                                                 R"({"launchClient":false})", token);
        check(response.status == 404, "play on missing game returns 404");
    }

    app.requestStop();
    serverThread.join();

    if (failures == 0) {
        std::cout << "web_server_tests passed" << std::endl;
        return 0;
    }
    std::cout << failures << " web_server_tests failed" << std::endl;
    return 1;
}
