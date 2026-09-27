#include "WebApp.h"

#include <csignal>
#include <iostream>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace {

web::WebApp* g_app = nullptr;

void handleSignal(int signal) {
    (void)signal;
    if (g_app) {
        g_app->requestStop();
    }
}

} // namespace

int main(int argc, char** argv) {
    int port = 0;
    std::string databasePath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--database" && i + 1 < argc) {
            databasePath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: WebServer [--port N] [--database path]\n"
                      << "Environment variables: PORT, JWT_SECRET, JWT_EXPIRES_IN, DATABASE_PATH,\n"
                      << "BCRYPT_ROUNDS, CORS_ORIGIN, GAME_WORLDS_DIR, GAME_SERVER_HOST,\n"
                      << "GAME_SERVER_BASE_PORT, GAME_INSTANCE_EMPTY_GRACE_MS, PUBLIC_BASE_URL,\n"
                      << "SERVER_EXECUTABLE_PATH, CLIENT_EXECUTABLE_PATH" << std::endl;
            return 0;
        }
    }

    web::WebApp::Config config = web::WebApp::configFromEnvironment();
    if (port > 0) config.port = port;
    if (!databasePath.empty()) config.databasePath = databasePath;

    web::WebApp app;
    g_app = &app;

#if !defined(_WIN32)
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#else
    std::signal(SIGINT, handleSignal);
#endif

    std::string error;
    if (!app.initialize(std::move(config), error)) {
        std::cerr << "Failed to start web server: " << error << std::endl;
        return 1;
    }
    if (!app.listen(error)) {
        std::cerr << "Failed to bind: " << error << std::endl;
        return 1;
    }
    return app.run();
}
