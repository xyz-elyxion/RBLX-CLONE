#include "WebApp.h"
#include "Db.h"
#include "WebCrypto.h"
#include "WebUtil.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace web {

namespace {

const char* kGameQueryBase =
    "SELECT g.id, g.owner_id, g.title, g.slug, g.description, g.is_public, "
    "g.world_path, g.icon_path, g.banner_path, g.parts_count, g.dynamic_parts_count, "
    "g.spawn_count, g.launch_count, g.created_at, g.updated_at, g.published_at, "
    "u.username AS owner_username "
    "FROM games g JOIN users u ON u.id = g.owner_id";

json serializeGame(Statement& row, bool includeOwnerControls) {
    json stats;
    stats["partsCount"] = row.getInt(9);
    stats["dynamicPartsCount"] = row.getInt(10);
    stats["spawnCount"] = row.getInt(11);
    stats["launchCount"] = row.getInt(12);

    json game;
    game["id"] = row.getInt(0);
    game["ownerId"] = row.getInt(1);
    game["ownerUsername"] = row.getText(16);
    game["title"] = row.getText(2);
    game["slug"] = row.getText(3);
    game["description"] = row.getText(4);
    game["isPublic"] = row.getInt(5) != 0;
    const std::string iconPath = row.getText(7);
    const std::string bannerPath = row.getText(8);
    game["iconUrl"] = iconPath.empty() ? util::kDefaultGameIcon : iconPath;
    game["bannerUrl"] = bannerPath.empty() ? util::kDefaultGameBanner : bannerPath;
    game["stats"] = stats;
    game["createdAt"] = row.isNull(13) ? json(nullptr) : json(row.getText(13));
    game["updatedAt"] = row.isNull(14) ? json(nullptr) : json(row.getText(14));
    game["publishedAt"] = row.isNull(15) ? json(nullptr) : json(row.getText(15));
    game["canEdit"] = includeOwnerControls;
    return game;
}

std::string normalizeGameTitle(const json& body, bool& ok) {
    ok = true;
    std::string title = util::collapseWhitespace(body.value("title", std::string()));
    if (title.size() < 3 || title.size() > 60) {
        ok = false;
        return "";
    }
    return title;
}

std::string normalizeGameDescription(const json& body) {
    std::string description = util::collapseWhitespace(body.value("description", std::string()));
    if (description.size() > 240) description.resize(240);
    return description;
}

} // namespace

// ---------------------------------------------------------------------------
// Game CRUD
// ---------------------------------------------------------------------------

HttpResponse WebApp::gamesList(const HttpRequest& request) {
    (void)request;
    json games = json::array();
    Statement stmt(db_, std::string(kGameQueryBase) +
                            " WHERE g.is_public = 1 ORDER BY g.published_at DESC, g.id DESC LIMIT 60");
    while (stmt.ok() && stmt.step()) {
        games.push_back(serializeGame(stmt, false));
    }
    return util::jsonResponse(200, json{{"success", true}, {"games", games}});
}

HttpResponse WebApp::gamesMine(const HttpRequest& request, int64_t userId) {
    (void)request;
    json games = json::array();
    Statement stmt(db_, std::string(kGameQueryBase) +
                            " WHERE g.owner_id = ? ORDER BY g.updated_at DESC, g.id DESC");
    stmt.bind(1, userId);
    while (stmt.ok() && stmt.step()) {
        games.push_back(serializeGame(stmt, true));
    }
    return util::jsonResponse(200, json{{"success", true}, {"games", games}});
}

bool WebApp::createOrUpdateGame(const json& body, int64_t userId, int64_t gameId, json& outGame,
                                HttpResponse& error) {
    bool titleOk = false;
    std::string title;
    if (body.contains("title")) {
        title = normalizeGameTitle(body, titleOk);
        if (!titleOk) {
            error = util::errorResponse(400, "Game title must be 3-60 characters.");
            return false;
        }
    }

    std::string worldText;
    bool hasWorld = false;
    if (body.contains("worldText") && body["worldText"].is_string()) {
        worldText = body["worldText"].get<std::string>();
        hasWorld = true;
    } else if (body.contains("world") && body["world"].is_string()) {
        worldText = body["world"].get<std::string>();
        hasWorld = true;
    }

    WorldAnalysis analysis;
    if (hasWorld) {
        std::string analysisError;
        if (!analyzeWorldSource(worldText, analysis, analysisError)) {
            error = util::errorResponse(400, analysisError);
            return false;
        }
    }

    const bool isPublic = util::toBool(body.contains("isPublic") ? body["isPublic"] : json(nullptr), true);

    if (gameId == 0) {
        const std::string slug = util::slugify(title);
        const std::string description = normalizeGameDescription(body);
        const std::string iconUrl = body.value("iconUrl", std::string(util::kDefaultGameIcon));
        const std::string bannerUrl = body.value("bannerUrl", std::string(util::kDefaultGameBanner));

        {
            Statement stmt(db_, "INSERT INTO games (owner_id, title, slug, description, is_public, world_path, "
                                "icon_path, banner_path, parts_count, dynamic_parts_count, spawn_count) "
                                "VALUES (?, ?, ?, ?, ?, '', ?, ?, ?, ?, ?)");
            stmt.bind(1, userId);
            stmt.bind(2, title);
            stmt.bind(3, slug);
            stmt.bind(4, description);
            stmt.bind(5, isPublic ? static_cast<int64_t>(1) : static_cast<int64_t>(0));
            stmt.bind(6, iconUrl);
            stmt.bind(7, bannerUrl);
            stmt.bind(8, static_cast<int64_t>(analysis.partsCount));
            stmt.bind(9, static_cast<int64_t>(analysis.dynamicPartsCount));
            stmt.bind(10, static_cast<int64_t>(analysis.spawnCount));
            if (!stmt.ok() || !stmt.exec()) {
                error = util::errorResponse(500, "Server error.");
                return false;
            }
        }
        gameId = lastInsertRowId(db_);
    } else {
        // Load existing row and verify ownership.
        int64_t ownerId = 0;
        std::string existingTitle;
        std::string existingDescription;
        int64_t existingPublic = 1;
        std::string existingIcon;
        std::string existingBanner;
        int64_t existingParts = 0;
        int64_t existingDynamic = 0;
        int64_t existingSpawn = 0;
        {
            Statement stmt(db_, "SELECT owner_id, title, description, is_public, icon_path, banner_path, "
                                "parts_count, dynamic_parts_count, spawn_count FROM games WHERE id = ?");
            stmt.bind(1, gameId);
            if (!stmt.ok() || !stmt.step() || stmt.getInt(0) != userId) {
                error = util::errorResponse(404, "Game not found.");
                return false;
            }
            ownerId = stmt.getInt(0);
            existingTitle = stmt.getText(1);
            existingDescription = stmt.getText(2);
            existingPublic = stmt.getInt(3);
            existingIcon = stmt.getText(4);
            existingBanner = stmt.getText(5);
            existingParts = stmt.getInt(6);
            existingDynamic = stmt.getInt(7);
            existingSpawn = stmt.getInt(8);
        }
        (void)ownerId;

        if (!body.contains("title")) title = existingTitle;
        const std::string description = body.contains("description")
                                            ? normalizeGameDescription(body)
                                            : existingDescription;
        const bool publicValue = body.contains("isPublic") ? isPublic : existingPublic != 0;
        const std::string iconUrl = body.contains("iconUrl")
                                        ? body.value("iconUrl", std::string(util::kDefaultGameIcon))
                                        : (existingIcon.empty() ? std::string(util::kDefaultGameIcon) : existingIcon);
        const std::string bannerUrl = body.contains("bannerUrl")
                                          ? body.value("bannerUrl", std::string(util::kDefaultGameBanner))
                                          : (existingBanner.empty() ? std::string(util::kDefaultGameBanner)
                                                                    : existingBanner);
        const int64_t partsCount = hasWorld ? analysis.partsCount : existingParts;
        const int64_t dynamicPartsCount = hasWorld ? analysis.dynamicPartsCount : existingDynamic;
        const int64_t spawnCount = hasWorld ? analysis.spawnCount : existingSpawn;

        Statement stmt(db_, "UPDATE games SET title = ?, slug = ?, description = ?, is_public = ?, "
                            "icon_path = ?, banner_path = ?, parts_count = ?, dynamic_parts_count = ?, "
                            "spawn_count = ?, updated_at = CURRENT_TIMESTAMP, published_at = CURRENT_TIMESTAMP "
                            "WHERE id = ?");
        stmt.bind(1, title);
        stmt.bind(2, util::slugify(title));
        stmt.bind(3, description);
        stmt.bind(4, publicValue ? static_cast<int64_t>(1) : static_cast<int64_t>(0));
        stmt.bind(5, iconUrl);
        stmt.bind(6, bannerUrl);
        stmt.bind(7, partsCount);
        stmt.bind(8, dynamicPartsCount);
        stmt.bind(9, spawnCount);
        stmt.bind(10, gameId);
        if (!stmt.ok() || !stmt.exec()) {
            error = util::errorResponse(500, "Server error.");
            return false;
        }
    }

    if (hasWorld) {
        const std::string worldPath = util::joinPath(
            config_.gameWorldsDir, "game-" + std::to_string(gameId) + "-" + std::to_string(util::nowMs()) + ".world");
        if (!util::writeFileAtomic(worldPath, analysis.source)) {
            error = util::errorResponse(500, "Server error.");
            return false;
        }
        Statement update(db_, "UPDATE games SET world_path = ? WHERE id = ?");
        update.bind(1, worldPath);
        update.bind(2, gameId);
        if (!update.ok() || !update.exec()) {
            error = util::errorResponse(500, "Server error.");
            return false;
        }
    }

    Statement stmt(db_, std::string(kGameQueryBase) + " WHERE g.id = ?");
    stmt.bind(1, gameId);
    if (!stmt.ok() || !stmt.step()) {
        error = util::errorResponse(500, "Server error.");
        return false;
    }
    outGame = serializeGame(stmt, true);
    return true;
}

HttpResponse WebApp::gamesCreate(const HttpRequest& request, int64_t userId) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    json game;
    if (!createOrUpdateGame(body, userId, 0, game, error)) return error;
    return util::jsonResponse(201, json{{"success", true}, {"game", game}});
}

HttpResponse WebApp::gamesPublish(const HttpRequest& request, int64_t userId) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    int64_t gameId = 0;
    if (body.contains("gameId") && body["gameId"].is_number() && body["gameId"].get<double>() > 0) {
        gameId = static_cast<int64_t>(body["gameId"].get<double>());
    }

    json game;
    const bool created = gameId == 0;
    if (!createOrUpdateGame(body, userId, gameId, game, error)) return error;
    return util::jsonResponse(created ? 201 : 200, json{{"success", true}, {"game", game}});
}

HttpResponse WebApp::gameDetail(const HttpRequest& request, int64_t viewerId, const std::string& idText) {
    (void)request;
    const int64_t gameId = util::parsePositiveInt(idText);
    if (gameId == 0) {
        return util::errorResponse(400, "Game ID must be a positive number.");
    }

    Statement stmt(db_, std::string(kGameQueryBase) + " WHERE g.id = ?");
    stmt.bind(1, gameId);
    if (!stmt.ok() || !stmt.step()) return util::notFound();

    const int64_t ownerId = stmt.getInt(1);
    const bool isPublic = stmt.getInt(5) != 0;
    if (!isPublic && viewerId != ownerId) return util::notFound();

    const bool canEdit = viewerId != 0 && viewerId == ownerId;
    json game = serializeGame(stmt, canEdit);

    json servers = json::array();
    for (const GameInstance& instance : activeInstancesForGame(gameId)) {
        servers.push_back(serializeInstance(instance));
    }
    return util::jsonResponse(200, json{{"success", true}, {"game", game}, {"servers", servers}});
}

HttpResponse WebApp::gameUpdate(const HttpRequest& request, int64_t userId, const std::string& idText) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    const int64_t gameId = util::parsePositiveInt(idText);
    if (gameId == 0) {
        return util::errorResponse(400, "Game ID must be a positive number.");
    }

    json game;
    if (!createOrUpdateGame(body, userId, gameId, game, error)) return error;
    return util::jsonResponse(200, json{{"success", true}, {"game", game}});
}

HttpResponse WebApp::gameServers(const HttpRequest& request, int64_t viewerId, const std::string& idText) {
    (void)request;
    const int64_t gameId = util::parsePositiveInt(idText);
    Statement stmt(db_, "SELECT owner_id, is_public FROM games WHERE id = ?");
    stmt.bind(1, gameId);
    if (!stmt.ok() || !stmt.step()) return util::notFound();
    if (stmt.getInt(1) == 0 && stmt.getInt(0) != viewerId) return util::notFound();

    json servers = json::array();
    for (const GameInstance& instance : activeInstancesForGame(gameId)) {
        servers.push_back(serializeInstance(instance));
    }
    return util::jsonResponse(200, json{{"success", true}, {"servers", servers}});
}

HttpResponse WebApp::gamePlay(const HttpRequest& request, int64_t userId, const std::string& idText) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    const int64_t gameId = util::parsePositiveInt(idText);
    if (gameId == 0) {
        return util::errorResponse(400, "Game ID must be a positive number.");
    }

    std::string worldPath;
    {
        Statement stmt(db_, std::string(kGameQueryBase) + " WHERE g.id = ?");
        stmt.bind(1, gameId);
        if (!stmt.ok() || !stmt.step()) return util::notFound();
        if (stmt.getInt(5) == 0 && stmt.getInt(1) != userId) return util::notFound();
        worldPath = stmt.getText(6);
    }

    const bool forceNew = body.contains("newServer") && body["newServer"].is_boolean() &&
                          body["newServer"].get<bool>();
    std::string instanceError;
    GameInstance* instance = ensureGameInstance(gameId, worldPath, forceNew, instanceError);
    if (!instance) {
        return util::errorResponse(500, instanceError);
    }
    reserveForLaunch(*instance);

    {
        Statement stmt(db_, "UPDATE games SET launch_count = launch_count + 1, "
                            "updated_at = CURRENT_TIMESTAMP WHERE id = ?");
        stmt.bind(1, gameId);
        if (stmt.ok()) stmt.exec();
    }

    std::string token = bearerToken(request);
    const std::string webServerUrl = config_.publicBaseUrl;
    const std::string clientPath =
        config_.clientExecutablePath.empty() ? std::string("Client.exe") : config_.clientExecutablePath;

    std::ostringstream args;
    args << "--server " << instance->host << " --port " << instance->port << " --web \"" << webServerUrl
         << "\" --token " << token << " --game " << gameId << " --connect";
    const std::string clientArgs = args.str();

    std::ostringstream params;
    params << "gameId=" << gameId << "&host=" << instance->host << "&port=" << instance->port
           << "&web=" << webServerUrl << "&token=" << token;

    json launch;
    launch["host"] = instance->host;
    launch["port"] = instance->port;
    launch["webServerUrl"] = webServerUrl;
    launch["protocolUrl"] = "limey://play?" + params.str();
    launch["command"] = "\"" + clientPath + "\" " + clientArgs;

    // Auto-launch client when configured, mirroring launchClientIfAvailable().
    json player;
    if (body.contains("launchClient") && body["launchClient"].is_boolean() &&
        !body["launchClient"].get<bool>()) {
        player = json{{"launched", false}, {"reason", "Client launch was skipped."}};
    } else if (config_.clientExecutablePath.empty() || !util::fileExists(config_.clientExecutablePath)) {
        player = json{{"launched", false}, {"reason", "Client executable was not found."}};
    } else {
#if defined(_WIN32)
        const std::string commandLine = "\"" + config_.clientExecutablePath + "\" " + clientArgs;
        std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back('\0');
        STARTUPINFOA startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};
        if (CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, config_.repoRoot.c_str(), &startupInfo, &processInfo)) {
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            player = json{{"launched", true}, {"processId", static_cast<uint64_t>(processInfo.dwProcessId)}};
        } else {
            player = json{{"launched", false}, {"reason", "Client executable could not be started."}};
        }
#else
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::setpgid(0, 0);
            if (::chdir(config_.repoRoot.c_str()) != 0) _exit(127);
            const int devNull = ::open("/dev/null", O_WRONLY);
            if (devNull >= 0) {
                ::dup2(devNull, STDOUT_FILENO);
                ::dup2(devNull, STDERR_FILENO);
            }
            execl(config_.clientExecutablePath.c_str(), config_.clientExecutablePath.c_str(),
                  "--server", instance->host.c_str(), "--port", std::to_string(instance->port).c_str(),
                  "--web", webServerUrl.c_str(), "--token", token.c_str(), "--game",
                  std::to_string(gameId).c_str(), "--connect", static_cast<char*>(nullptr));
            _exit(127);
        }
        player = pid > 0 ? json{{"launched", true}, {"processId", static_cast<uint64_t>(pid)}}
                         : json{{"launched", false}, {"reason", "Client executable could not be started."}};
#endif
    }

    // Serialize the instance again (playerCount/emptySince may have changed).
    json serverJson;
    {
        std::lock_guard<std::mutex> lock(instancesMutex_);
        for (const GameInstance& current : instances_) {
            if (current.id == instance->id) {
                serverJson = serializeInstance(current);
                break;
            }
        }
    }

    json gameJson;
    {
        Statement stmt(db_, std::string(kGameQueryBase) + " WHERE g.id = ?");
        stmt.bind(1, gameId);
        if (stmt.ok() && stmt.step()) gameJson = serializeGame(stmt, stmt.getInt(1) == userId);
    }

    return util::jsonResponse(200, json{{"success", true},
                                        {"game", gameJson},
                                        {"server", serverJson},
                                        {"launch", launch},
                                        {"player", player}});
}

HttpResponse WebApp::heartbeat(const HttpRequest& request, const std::string& instanceId) {
    GameInstance* instance = nullptr;
    {
        std::lock_guard<std::mutex> lock(instancesMutex_);
        for (GameInstance& current : instances_) {
            if (current.id == instanceId) {
                instance = &current;
                break;
            }
        }
    }
    if (!instance) return util::errorResponse(404, "Game instance not found.");

    const std::string managerToken = bearerToken(request);
    if (managerToken.empty() || managerToken != instance->managerToken) {
        return util::errorResponse(403, "Game instance token is invalid.");
    }

    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    int playerCount = -1;
    if (body.contains("playerCount") && body["playerCount"].is_number()) {
        const double raw = body["playerCount"].get<double>();
        if (raw >= 0 && raw <= 200 && raw == static_cast<int>(raw)) {
            playerCount = static_cast<int>(raw);
        }
    }
    if (playerCount < 0) {
        return util::errorResponse(400, "Player count must be a number from 0 to 200.");
    }

    {
        std::lock_guard<std::mutex> lock(instancesMutex_);
        updateHeartbeat(*instance, playerCount);
    }
    json serverJson;
    {
        std::lock_guard<std::mutex> lock(instancesMutex_);
        serverJson = serializeInstance(*instance);
    }
    return util::jsonResponse(200, json{{"success", true}, {"server", serverJson}});
}

} // namespace web
