#include "WebApp.h"
#include "Db.h"
#include "WebCrypto.h"
#include "WebUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <cstdlib>
#endif

namespace web {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

WebApp::Config WebApp::configFromEnvironment() {
    Config config;

    auto env = [](const char* name) -> std::string {
        const char* value =
#if defined(_WIN32)
            std::getenv(name);
#else
            ::getenv(name);
#endif
        return value ? std::string(value) : std::string();
    };

    const std::string nodeEnv = env("NODE_ENV");
    config.nodeEnv = nodeEnv.empty() ? "development" : nodeEnv;
    config.isProduction = config.nodeEnv == "production";

    const std::string portValue = env("PORT");
    if (!portValue.empty()) {
        const int parsed = std::atoi(portValue.c_str());
        if (parsed > 0) config.port = parsed;
    }

    config.jwtSecret = env("JWT_SECRET");
    if (config.jwtSecret.empty() && !config.isProduction) {
        config.jwtSecret = "local-dev-secret-change-me";
    }

    const std::string expires = env("JWT_EXPIRES_IN");
    if (!expires.empty()) {
        config.jwtExpiresInSeconds = parseJwtExpiresInSeconds(expires, config.jwtExpiresInSeconds);
    }

    if (const std::string value = env("DATABASE_PATH"); !value.empty()) config.databasePath = value;
    if (const std::string value = env("BCRYPT_ROUNDS"); !value.empty()) {
        const int parsed = std::atoi(value.c_str());
        if (parsed > 0 && parsed <= 15) config.bcryptRounds = parsed;
    } else if (config.nodeEnv == "test") {
        config.bcryptRounds = 4;
    }
    if (const std::string value = env("AUTH_RATE_LIMIT_WINDOW_MS"); !value.empty()) {
        const int parsed = std::atoi(value.c_str());
        if (parsed > 0) config.authRateLimitWindowMs = parsed;
    }
    if (const std::string value = env("AUTH_RATE_LIMIT_MAX"); !value.empty()) {
        const int parsed = std::atoi(value.c_str());
        if (parsed > 0) config.authRateLimitMax = parsed;
    }

    if (const std::string value = env("GAME_SERVER_HOST"); !value.empty()) config.gameServerHost = value;
    if (const std::string value = env("GAME_SERVER_BASE_PORT"); !value.empty()) {
        const int parsed = std::atoi(value.c_str());
        if (parsed > 0) config.gameServerBasePort = parsed;
    }
    if (const std::string value = env("GAME_INSTANCE_EMPTY_GRACE_MS"); !value.empty()) {
        const int parsed = std::atoi(value.c_str());
        if (parsed > 0) config.gameInstanceEmptyGraceMs = parsed;
    }
    if (const std::string value = env("PUBLIC_BASE_URL"); !value.empty()) config.publicBaseUrl = value;
    if (const std::string value = env("SERVER_EXECUTABLE_PATH"); !value.empty()) config.serverExecutablePath = value;
    if (const std::string value = env("CLIENT_EXECUTABLE_PATH"); !value.empty()) config.clientExecutablePath = value;

    if (const std::string value = env("CORS_ORIGIN"); !value.empty()) {
        std::string item;
        std::istringstream stream(value);
        while (std::getline(stream, item, ',')) {
            const std::string origin = util::trim(item);
            if (!origin.empty()) config.corsOrigins.push_back(origin);
        }
    }

    return config;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

WebApp::WebApp() {
    rngState_ = static_cast<uint64_t>(util::nowMs()) ^ 0x9e3779b97f4a7c15ULL;
}

WebApp::~WebApp() {
    requestStop();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool WebApp::initialize(Config config, std::string& error) {
    config_ = std::move(config);

    if (config_.repoRoot.empty()) {
        config_.repoRoot = std::filesystem::current_path().string();
    }
    if (config_.publicDir.empty()) {
        config_.publicDir = util::joinPath(config_.repoRoot, "web/public");
    }
    if (config_.vendorThreeDir.empty()) {
        config_.vendorThreeDir = util::joinPath(config_.repoRoot, "web/vendor/three");
    }
    if (config_.gameWorldsDir.empty()) {
        config_.gameWorldsDir = util::joinPath(config_.repoRoot, "web/game-worlds");
    }
    if (config_.publicBaseUrl.empty()) {
        config_.publicBaseUrl = "http://localhost:" + std::to_string(config_.port);
    }
    if (config_.databasePath != ":memory:" && config_.databasePath.find('/') == std::string::npos &&
        config_.databasePath.find('\\') == std::string::npos) {
        config_.databasePath = util::joinPath(config_.repoRoot, "web/" + config_.databasePath);
    }

    const std::string databaseParent = std::filesystem::path(config_.databasePath).parent_path().string();
    if (config_.databasePath != ":memory:" && !databaseParent.empty() &&
        !util::ensureDirectory(databaseParent)) {
        error = "Could not create database directory.";
        return false;
    }
    if (!util::ensureDirectory(config_.gameWorldsDir)) {
        error = "Could not create game worlds directory.";
        return false;
    }

    if (sqlite3_open(config_.databasePath.c_str(), &db_) != SQLITE_OK) {
        error = "Could not open database: " + config_.databasePath;
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);

    if (!ensureSchema(error)) return false;
    seedStarterContent();

    server_.addRoute("GET", "/api/*", &WebApp::handleApi);
    server_.addRoute("POST", "/api/*", &WebApp::handleApi);
    server_.addRoute("PUT", "/api/*", &WebApp::handleApi);
    server_.addRoute("DELETE", "/api/*", &WebApp::handleApi);
    server_.setFallback(&WebApp::handleFallback);
    server_.setUser(this);

    return true;
}

bool WebApp::listen(std::string& error) {
    if (!server_.listen("0.0.0.0", config_.port, error)) return false;
    std::cout << "Limey web server (C++) listening on http://localhost:" << config_.port << std::endl;
    return true;
}

int WebApp::run() {
    reaperThread_ = std::thread(&WebApp::instanceReaperLoop, this);
    server_.run();
    return 0;
}

void WebApp::requestStop() {
    stopRequested_ = true;
    server_.stop();
    if (reaperThread_.joinable()) {
        reaperThread_.join();
    }
}

bool WebApp::ensureSchema(std::string& error) {
    const char* statements[] = {
        "CREATE TABLE IF NOT EXISTS users (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    username TEXT UNIQUE NOT NULL,\n"
        "    password_hash TEXT NOT NULL,\n"
        "    created_at DATETIME DEFAULT CURRENT_TIMESTAMP\n"
        ")",
        "CREATE TABLE IF NOT EXISTS avatars (\n"
        "    user_id INTEGER PRIMARY KEY,\n"
        "    head_color_r REAL DEFAULT 0.8,\n"
        "    head_color_g REAL DEFAULT 0.6,\n"
        "    head_color_b REAL DEFAULT 0.4,\n"
        "    torso_color_r REAL DEFAULT 0.2,\n"
        "    torso_color_g REAL DEFAULT 0.4,\n"
        "    torso_color_b REAL DEFAULT 0.8,\n"
        "    left_arm_color_r REAL DEFAULT 0.8,\n"
        "    left_arm_color_g REAL DEFAULT 0.6,\n"
        "    left_arm_color_b REAL DEFAULT 0.4,\n"
        "    right_arm_color_r REAL DEFAULT 0.8,\n"
        "    right_arm_color_g REAL DEFAULT 0.6,\n"
        "    right_arm_color_b REAL DEFAULT 0.4,\n"
        "    left_leg_color_r REAL DEFAULT 0.2,\n"
        "    left_leg_color_g REAL DEFAULT 0.6,\n"
        "    left_leg_color_b REAL DEFAULT 0.2,\n"
        "    right_leg_color_r REAL DEFAULT 0.2,\n"
        "    right_leg_color_g REAL DEFAULT 0.6,\n"
        "    right_leg_color_b REAL DEFAULT 0.2,\n"
        "    face_id TEXT DEFAULT 'classic',\n"
        "    FOREIGN KEY (user_id) REFERENCES users(id)\n"
        ")",
        "CREATE TABLE IF NOT EXISTS friendships (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    requester_id INTEGER NOT NULL,\n"
        "    addressee_id INTEGER NOT NULL,\n"
        "    status TEXT NOT NULL CHECK(status IN ('pending', 'accepted')),\n"
        "    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
        "    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
        "    UNIQUE(requester_id, addressee_id),\n"
        "    CHECK(requester_id <> addressee_id),\n"
        "    FOREIGN KEY (requester_id) REFERENCES users(id),\n"
        "    FOREIGN KEY (addressee_id) REFERENCES users(id)\n"
        ")",
        "CREATE TABLE IF NOT EXISTS user_stats (\n"
        "    user_id INTEGER PRIMARY KEY,\n"
        "    playtime_seconds INTEGER NOT NULL DEFAULT 0,\n"
        "    last_played_at DATETIME,\n"
        "    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
        "    FOREIGN KEY (user_id) REFERENCES users(id)\n"
        ")",
        "CREATE TABLE IF NOT EXISTS games (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    owner_id INTEGER NOT NULL,\n"
        "    title TEXT NOT NULL,\n"
        "    slug TEXT NOT NULL,\n"
        "    description TEXT NOT NULL DEFAULT '',\n"
        "    is_public INTEGER NOT NULL DEFAULT 1,\n"
        "    world_path TEXT NOT NULL,\n"
        "    icon_path TEXT NOT NULL DEFAULT '/assets/games/default-game-icon.png',\n"
        "    banner_path TEXT NOT NULL DEFAULT '/assets/games/default-game-banner.png',\n"
        "    parts_count INTEGER NOT NULL DEFAULT 0,\n"
        "    dynamic_parts_count INTEGER NOT NULL DEFAULT 0,\n"
        "    spawn_count INTEGER NOT NULL DEFAULT 0,\n"
        "    launch_count INTEGER NOT NULL DEFAULT 0,\n"
        "    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
        "    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
        "    published_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
        "    FOREIGN KEY (owner_id) REFERENCES users(id)\n"
        ")"
    };

    for (const char* sql : statements) {
        char* errorMessage = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
            error = errorMessage ? errorMessage : "SQLite schema error.";
            sqlite3_free(errorMessage);
            return false;
        }
    }
    return true;
}

void WebApp::seedStarterContent() {
    int64_t gameCount = 0;
    {
        Statement stmt(db_, "SELECT COUNT(*) FROM games");
        if (stmt.ok() && stmt.step()) gameCount = stmt.getInt(0);
    }
    if (gameCount > 0) return;

    int64_t ownerId = 0;
    {
        Statement stmt(db_, "SELECT id FROM users ORDER BY id LIMIT 1");
        if (stmt.ok() && stmt.step()) ownerId = stmt.getInt(0);
    }
    if (ownerId == 0) {
        const std::string hash = bcryptHash("local-starter-account", config_.bcryptRounds);
        Statement stmt(db_, "INSERT INTO users (username, password_hash) VALUES (?, ?)");
        stmt.bind(1, "LimeyStarter");
        stmt.bind(2, hash);
        if (stmt.ok() && stmt.exec()) {
            ownerId = lastInsertRowId(db_);
        }
    }

    struct Seed {
        std::string path;
        std::string title;
        std::string description;
    };
    const std::vector<Seed> seeds = {
        {util::joinPath(config_.repoRoot, "ServerWorld.world"),
         "Starter Baseplate",
         "The default shared world, now published as the first public game."},
        {util::joinPath(config_.repoRoot, "web/seed-worlds/separate-server-test.world"),
         "Separate Server Test Arena",
         "A second published world for testing that each game starts its own local server instance."}
    };

    for (const Seed& seed : seeds) {
        std::string worldText;
        if (!util::readFile(seed.path, worldText)) continue;

        WorldAnalysis analysis;
        std::string analysisError;
        if (!analyzeWorldSource(worldText, analysis, analysisError)) continue;

        Statement stmt(db_,
                       "INSERT INTO games (owner_id, title, slug, description, is_public, world_path, "
                       "icon_path, banner_path, parts_count, dynamic_parts_count, spawn_count) "
                       "VALUES (?, ?, ?, ?, 1, '', ?, ?, ?, ?, ?)");
        stmt.bind(1, ownerId);
        stmt.bind(2, seed.title);
        stmt.bind(3, util::slugify(seed.title));
        stmt.bind(4, seed.description);
        stmt.bind(5, util::kDefaultGameIcon);
        stmt.bind(6, util::kDefaultGameBanner);
        stmt.bind(7, static_cast<int64_t>(analysis.partsCount));
        stmt.bind(8, static_cast<int64_t>(analysis.dynamicPartsCount));
        stmt.bind(9, static_cast<int64_t>(analysis.spawnCount));
        if (!stmt.ok() || !stmt.exec()) continue;

        const int64_t gameId = lastInsertRowId(db_);
        const std::string worldPath = util::joinPath(
            config_.gameWorldsDir, "game-" + std::to_string(gameId) + "-" + std::to_string(util::nowMs()) + ".world");
        util::writeFileAtomic(worldPath, analysis.source);

        Statement update(db_, "UPDATE games SET world_path = ? WHERE id = ?");
        update.bind(1, worldPath);
        update.bind(2, gameId);
        if (update.ok()) update.exec();
    }
}

} // namespace web
