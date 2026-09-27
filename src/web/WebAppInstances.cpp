#include "WebApp.h"
#include "WebCrypto.h"
#include "WebUtil.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

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
    #include <signal.h>
    #include <unistd.h>
#endif

namespace web {

namespace {

std::string randomHex(uint64_t& state, std::mutex& mutex, size_t length) {
    std::lock_guard<std::mutex> lock(mutex);
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        out += "0123456789abcdef"[(state >> 33) & 0xf];
    }
    return out;
}

} // namespace

bool WebApp::isProcessAlive(const GameInstance& instance) const {
    if (!instance.hasProcess) return false;
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(instance.processHandle);
    if (!handle) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(handle, &code)) return false;
    return code == STILL_ACTIVE;
#else
    return ::kill(static_cast<pid_t>(instance.processId), 0) == 0;
#endif
}

void WebApp::terminateProcess(GameInstance& instance) {
    if (!instance.hasProcess) return;
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(instance.processHandle);
    if (handle) {
        TerminateProcess(handle, 0);
        CloseHandle(handle);
    }
#else
    ::kill(static_cast<pid_t>(instance.processId), SIGTERM);
#endif
    instance.hasProcess = false;
    instance.childRunning = false;
}

bool WebApp::spawnGameServer(GameInstance& instance, const std::string& worldPath) {
    if (config_.serverExecutablePath.empty() || !util::fileExists(config_.serverExecutablePath)) {
        instance.command = "Server.exe --port " + std::to_string(instance.port) + " --world \"" + worldPath +
                           "\" --web \"" + config_.publicBaseUrl + "\"";
        return false;
    }

    const std::string args = "--port " + std::to_string(instance.port) + " --world \"" + worldPath +
                             "\" --web \"" + config_.publicBaseUrl + "\" --instance " + instance.id +
                             " --instance-token " + instance.managerToken;
    instance.command = "\"" + config_.serverExecutablePath + "\" " + args;

#if defined(_WIN32)
    const std::string commandLine = "\"" + config_.serverExecutablePath + "\" " + args;
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, config_.repoRoot.c_str(), &startupInfo, &processInfo)) {
        return false;
    }
    CloseHandle(processInfo.hThread);
    instance.processHandle = reinterpret_cast<uint64_t>(processInfo.hProcess);
    instance.processId = static_cast<uint64_t>(processInfo.dwProcessId);
    instance.hasProcess = true;
    instance.childRunning = true;
#else
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setpgid(0, 0);
        if (::chdir(config_.repoRoot.c_str()) != 0) _exit(127);
        const int devNull = ::open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
        }
        execl(config_.serverExecutablePath.c_str(), config_.serverExecutablePath.c_str(),
              "--port", std::to_string(instance.port).c_str(), "--world", worldPath.c_str(),
              "--web", config_.publicBaseUrl.c_str(), "--instance", instance.id.c_str(),
              "--instance-token", instance.managerToken.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    instance.processId = static_cast<uint64_t>(pid);
    instance.hasProcess = true;
    instance.childRunning = true;
#endif
    return true;
}

std::vector<WebApp::GameInstance> WebApp::activeInstancesForGame(int64_t gameId) {
    std::lock_guard<std::mutex> lock(instancesMutex_);
    std::vector<GameInstance> active;
    for (const GameInstance& instance : instances_) {
        if (instance.gameId != gameId) continue;
        const bool running = instance.manual || instance.childRunning || isProcessAlive(instance);
        if (!running) continue;
        active.push_back(instance);
    }
    return active;
}

nlohmann::json WebApp::serializeInstance(const GameInstance& instance) const {
    return nlohmann::json{
        {"id", instance.id},
        {"host", instance.host},
        {"port", instance.port},
        {"status", instance.manual ? "manual" : instance.status},
        {"processId", instance.processId == 0 ? nlohmann::json(nullptr) : nlohmann::json(instance.processId)},
        {"startedAt", instance.startedAt},
        {"playerCount", instance.playerCount},
        {"lastHeartbeatAt", instance.lastHeartbeatAt.empty() ? nlohmann::json(nullptr)
                                                             : nlohmann::json(instance.lastHeartbeatAt)},
        {"emptySince", instance.emptySince.empty() ? nlohmann::json(nullptr) : nlohmann::json(instance.emptySince)},
        {"emptyShutdownAt", instance.emptyShutdownAt.empty() ? nlohmann::json(nullptr)
                                                             : nlohmann::json(instance.emptyShutdownAt)}
    };
}

WebApp::GameInstance* WebApp::ensureGameInstance(int64_t gameId, const std::string& worldPath, bool forceNew,
                                                 std::string& errorMessage) {
    std::lock_guard<std::mutex> lock(instancesMutex_);

    if (!forceNew) {
        for (GameInstance& instance : instances_) {
            if (instance.gameId == gameId && (instance.manual || instance.childRunning)) {
                return &instance;
            }
        }
    }

    // Find an available port, mirroring findAvailablePort() in web/server.js.
    int port = 0;
    for (int offset = 0; offset < 200; ++offset) {
        const int candidate = config_.gameServerBasePort + static_cast<int>(offset);
        bool usedByInstance = false;
        for (const GameInstance& instance : instances_) {
            if (instance.port == candidate) {
                usedByInstance = true;
                break;
            }
        }
        if (usedByInstance) continue;
        port = candidate;
        break;
    }
    if (port == 0) {
        errorMessage = "No local game server ports are available.";
        return nullptr;
    }

    instances_.emplace_back();
    GameInstance& instance = instances_.back();
    instance.id = randomHex(rngState_, rngMutex_, 32);
    instance.gameId = gameId;
    instance.host = config_.gameServerHost;
    instance.port = port;
    instance.status = "starting";
    instance.manual = false;
    instance.startedAt = util::nowIso();
    instance.playerCount = 0;
    instance.managerToken = randomHex(rngState_, rngMutex_, 48);

    if (!spawnGameServer(instance, worldPath)) {
        // No executable available: still expose the command so the site can
        // show how to launch manually (same behavior as the Node server).
        instance.status = "manual";
        instance.manual = true;
    }
    return &instance;
}

void WebApp::reserveForLaunch(GameInstance& instance) {
    if (instance.manual || instance.playerCount > 0) return;
    instance.emptySince = util::nowIso();
    instance.emptySinceMs = util::nowMs();
    instance.emptyShutdownAt = util::nowIso();
    instance.shutdownScheduled = true;
}

void WebApp::updateHeartbeat(GameInstance& instance, int playerCount) {
    instance.playerCount = playerCount;
    instance.lastHeartbeatAt = util::nowIso();

    if (playerCount > 0) {
        if (!instance.manual) instance.status = "running";
        instance.emptySince.clear();
        instance.emptyShutdownAt.clear();
        instance.shutdownScheduled = false;
        instance.emptySinceMs = 0;
        return;
    }

    if (instance.emptySince.empty()) {
        instance.emptySince = util::nowIso();
        instance.emptySinceMs = util::nowMs();
    }
    instance.shutdownScheduled = true;
}

void WebApp::shutdownEmptyInstances() {
    std::vector<size_t> toErase;
    {
        std::lock_guard<std::mutex> lock(instancesMutex_);
        const int64_t now = util::nowMs();
        for (size_t i = 0; i < instances_.size(); ++i) {
            GameInstance& instance = instances_[i];
            const bool running = instance.manual || isProcessAlive(instance);
            if (!running) {
                toErase.push_back(i);
                continue;
            }
            if (instance.manual || !instance.shutdownScheduled || instance.playerCount > 0) continue;
            if (instance.emptySinceMs == 0) continue;

            const int64_t emptyFor = now - instance.emptySinceMs;
            if (emptyFor >= config_.gameInstanceEmptyGraceMs) {
                terminateProcess(instance);
                toErase.push_back(i);
            }
        }
        for (size_t i = toErase.size(); i-- > 0;) {
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(toErase[i]));
        }
    }
}

void WebApp::instanceReaperLoop() {
    while (!stopRequested_) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (stopRequested_) break;
        shutdownEmptyInstances();
    }
}

} // namespace web
