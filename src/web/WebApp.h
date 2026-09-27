#pragma once

#include "HttpServer.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace web {

class WebApp {
public:
    struct Config {
        std::string nodeEnv = "development";
        bool isProduction = false;
        int port = 3000;
        std::string jwtSecret = "local-dev-secret-change-me";
        int64_t jwtExpiresInSeconds = 7 * 24 * 3600;
        std::string databasePath = "users.db";
        int bcryptRounds = 10;
        std::string publicDir;             // defaults: <repo>/web/public
        std::string vendorThreeDir;        // defaults: <repo>/web/vendor/three
        std::string gameWorldsDir;         // defaults: <repo>/web/game-worlds
        std::string repoRoot;              // defaults: current working directory
        std::string gameServerHost = "127.0.0.1";
        int gameServerBasePort = 7777;
        int64_t gameInstanceEmptyGraceMs = 30 * 1000;
        std::string publicBaseUrl;         // defaults: http://localhost:<port>
        std::string serverExecutablePath;  // empty = launcher without executable
        std::string clientExecutablePath;  // empty = do not auto-launch client
        std::vector<std::string> corsOrigins;
        int authRateLimitWindowMs = 15 * 60 * 1000;
        int authRateLimitMax = 20;
    };

    WebApp();
    ~WebApp();

    WebApp(const WebApp&) = delete;
    WebApp& operator=(const WebApp&) = delete;

    // Builds the config from environment variables with the same names and
    // defaults as the previous Node server (PORT, JWT_SECRET, ...).
    static Config configFromEnvironment();

    bool initialize(Config config, std::string& error);
    bool listen(std::string& error);
    int run();                      // accept loop; blocks
    void requestStop();

    const Config& config() const { return config_; }

private:
    struct GameInstance {
        std::string id;
        int64_t gameId = 0;
        std::string host;
        int port = 0;
        std::string status;            // starting / running / stopping / error / manual
        bool manual = false;
        std::string startedAt;
        int playerCount = 0;
        std::string lastHeartbeatAt;
        std::string emptySince;
        std::string emptyShutdownAt;
        std::string managerToken;
        uint64_t processId = 0;
        uint64_t processHandle = 0;
        bool hasProcess = false;
        bool childRunning = false;
        std::string command;
        int64_t emptySinceMs = 0;
        bool shutdownScheduled = false;
        bool stoppedByManager = false;
    };

    // --- route handlers (static, forwarded to instance) ---
    static HttpResponse handleApi(const HttpRequest& request, void* user, const std::vector<std::string>& captures);
    static HttpResponse handleFallback(const HttpRequest& request, void* user);

    HttpResponse onApi(const HttpRequest& request, const std::vector<std::string>& captures);
    HttpResponse onFallback(const HttpRequest& request);
    HttpResponse serveStaticFile(const std::string& urlPath);
    std::string contentTypeFor(const std::string& path) const;

    // --- internals ---
    bool ensureSchema(std::string& error);
    void seedStarterContent();

    // auth helpers
    std::string bearerToken(const HttpRequest& request) const;
    bool authenticate(const HttpRequest& request, int64_t& userId, std::string& username,
                      HttpResponse& errorResponse) const;
    bool optionalAuthenticate(const HttpRequest& request, int64_t& userId, std::string& username) const;
    bool issueToken(int64_t userId, const std::string& username, std::string& token) const;

    // db helpers
    bool jsonBody(const HttpRequest& request, nlohmann::json& body, HttpResponse& errorResponse) const;
    bool requireJsonContent(const HttpRequest& request, HttpResponse& errorResponse) const;

    // endpoint implementations
    HttpResponse signup(const HttpRequest& request);
    HttpResponse login(const HttpRequest& request);
    HttpResponse verify(const HttpRequest& request);
    HttpResponse meSocial(const HttpRequest& request, int64_t userId);
    HttpResponse mePlaytime(const HttpRequest& request, int64_t userId);
    HttpResponse heartbeat(const HttpRequest& request, const std::string& instanceId);
    HttpResponse usersSearch(const HttpRequest& request, int64_t userId);
    HttpResponse userDetail(const HttpRequest& request, int64_t userId, const std::string& idText);
    HttpResponse friendsRequest(const HttpRequest& request, int64_t userId);
    HttpResponse friendsRespond(const HttpRequest& request, int64_t userId);
    HttpResponse friendsRemove(const HttpRequest& request, int64_t userId);
    HttpResponse gamesList(const HttpRequest& request);
    HttpResponse gamesMine(const HttpRequest& request, int64_t userId);
    HttpResponse gamesCreate(const HttpRequest& request, int64_t userId);
    HttpResponse gamesPublish(const HttpRequest& request, int64_t userId);
    HttpResponse gameDetail(const HttpRequest& request, int64_t viewerId, const std::string& idText);
    HttpResponse gameUpdate(const HttpRequest& request, int64_t userId, const std::string& idText);
    HttpResponse gameServers(const HttpRequest& request, int64_t viewerId, const std::string& idText);
    HttpResponse gamePlay(const HttpRequest& request, int64_t userId, const std::string& idText);
    HttpResponse avatarGet(const HttpRequest& request, int64_t userId);
    HttpResponse avatarPost(const HttpRequest& request, int64_t userId);

    // Shared by create (gameId == 0), publish, and update paths.
    bool createOrUpdateGame(const nlohmann::json& body, int64_t userId, int64_t gameId,
                            nlohmann::json& outGame, HttpResponse& error);

    // world analysis (parity with web/server.js)
    struct WorldAnalysis {
        std::string source;
        int partsCount = 0;
        int dynamicPartsCount = 0;
        int spawnCount = 0;
    };
    bool analyzeWorldSource(const std::string& worldText, WorldAnalysis& analysis, std::string& errorMessage) const;

    // game instance manager
    std::vector<GameInstance> activeInstancesForGame(int64_t gameId);
    nlohmann::json serializeInstance(const GameInstance& instance) const;
    GameInstance* ensureGameInstance(int64_t gameId, const std::string& worldPath, bool forceNew,
                                     std::string& errorMessage);
    void reserveForLaunch(GameInstance& instance);
    void updateHeartbeat(GameInstance& instance, int playerCount);
    void shutdownEmptyInstances();
    void instanceReaperLoop();
    bool spawnGameServer(GameInstance& instance, const std::string& worldPath);
    void terminateProcess(GameInstance& instance);
    bool isProcessAlive(const GameInstance& instance) const;

    Config config_;
    sqlite3* db_ = nullptr;
    HttpServer server_;
    std::mutex dbMutex_;
    std::mutex instancesMutex_;
    std::vector<GameInstance> instances_;
    std::atomic<bool> stopRequested_{false};
    std::thread reaperThread_;
    uint64_t rngState_ = 0;
    std::mutex rngMutex_;
};

} // namespace web
