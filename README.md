# Limey

Limey is an early prototype of a Roblox-style client, game server, Studio editor, and web authentication/avatar service. It is usable for local experimentation, but it is not production-ready.

## Current Status

- Client, Server, Studio, and the web backend are C++17 targets.
- The Client and Server currently require Windows because they use WinSock and WinHTTP. The web backend (WebServer) is fully cross-platform.
- Studio uses GLFW/OpenGL/ImGui/ImGuizmo and has a basic world editing path.
- The web backend is a native C++ service (no Node.js) with SQLite, bcrypt password hashing, JWT authentication, avatar storage, public game catalog pages, and local game-server launch orchestration. It serves the exact same HTTP API as the original Node prototype, so existing clients and saved databases keep working.
- Networking is still a compact binary protocol. It now has shared framing, payload limits, partial send handling, and receive buffering, but it is still a prototype protocol.

## Repository Layout

- `CMakeLists.txt` - C++ build configuration.
- `CMakePresets.json` - Visual Studio debug preset for Windows.
- `include/` - C++ headers.
- `src/` - C++ Client, Server, Studio, web backend, rendering, physics, auth, and world loading code.
- `shaders/` - OpenGL shader files used by Client and Studio.
- `tests/` - C++ protocol and web server tests.
- `vendor/` - Vendored third-party sources (SQLite, nlohmann/json, crypt_blowfish).
- `web/` - Public HTML/JS/CSS pages served by the native web server.
- `ServerWorld.world` - Default world file.
- `GameRelease/` - Existing release asset folders. Do not commit generated binaries here.

## Prerequisites

### C++ Targets

Windows is currently required for Client and Server. The web backend builds anywhere.

- Visual Studio 2026/2022 with "Desktop development with C++" (or a MinGW/Ninja toolchain).
- CMake 3.21 or newer if using `CMakePresets.json`; CMake 3.14 or newer for manual configure.
- OpenGL-capable GPU/drivers for running Studio/Client.
- Linux: `libgl1-mesa-dev libx11-dev libxi-dev libxcursor-dev libxrandr-dev libxinerama-dev` for GLFW.

There is no Node.js, npm, or any other runtime dependency: SQLite, nlohmann/json, and bcrypt are vendored under `vendor/` and built from source.

## Build C++

From a Developer PowerShell or Developer Command Prompt:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Manual configure also works:

```powershell
cmake -S . -B build/windows-debug -G "Visual Studio 17 2022" -DRBLX_BUILD_TESTS=ON
cmake --build build/windows-debug --config Debug
ctest --test-dir build/windows-debug -C Debug
```

### Docker (web server)

The web backend ships as a Docker image that compiles the server and runs its test suite during the image build, then serves the site from a minimal Debian runtime:

```bash
docker build -t limey-webserver .
docker run --rm -p 3000:3000 \
  -e JWT_SECRET="replace-with-a-long-random-secret" \
  -v limey-data:/data \
  limey-webserver
```

The server listens on `0.0.0.0:3000` (override with `PORT`). Accounts, published games, and spawned world files live under the `/data` volume (`DATABASE_PATH=/data/users.db`, `GAME_WORLDS_DIR=/data/game-worlds`), so they survive container replacement. `NODE_ENV=production` is set in the image and the server refuses to start without a `JWT_SECRET`.

### Build the web server directly (no Docker)

The web server and its tests build on any platform (no OpenGL needed):

```powershell
cmake -S . -B build/web -G Ninja -DRBLX_BUILD_CLIENT=OFF -DRBLX_BUILD_SERVER=OFF -DRBLX_BUILD_STUDIO=OFF
cmake --build build/web
ctest --test-dir build/web --output-on-failure
```

The project has also been verified with a portable MinGW/Ninja toolchain:

```powershell
cmake -S . -B build/ninja -G Ninja -DCMAKE_C_COMPILER=C:\path\to\gcc.exe -DCMAKE_CXX_COMPILER=C:\path\to\g++.exe -DRBLX_BUILD_TESTS=ON
cmake --build build/ninja
ctest --test-dir build/ninja --output-on-failure
```

Useful CMake options:

- `RBLX_BUILD_CLIENT=ON|OFF`
- `RBLX_BUILD_SERVER=ON|OFF`
- `RBLX_BUILD_STUDIO=ON|OFF`
- `RBLX_BUILD_TESTS=ON|OFF`
- `RBLX_ENABLE_WARNINGS=ON|OFF`

On non-Windows hosts, CMake disables Client and Server because those targets depend on WinSock/WinHTTP.

## Run C++ Targets

After building Debug with the preset:

```powershell
.\build\windows-debug\Debug\Studio.exe
.\build\windows-debug\Debug\Server.exe
.\build\windows-debug\Debug\Client.exe
```

Start the web backend before using authenticated Client login.

## Web Backend Setup

Build the `WebServer` target (see "Build C++" above), then run it:

```powershell
.\build\windows-debug\Debug\WebServer.exe
```

The backend listens on `http://localhost:3000` by default and serves the site from `web/public`.

Configure it with environment variables (or copy `web/.env.example` and load it yourself). Do not commit `.env`.

Environment variables:

- `NODE_ENV` - `development`, `test`, or `production`.
- `PORT` - HTTP port, default `3000`.
- `DATABASE_PATH` - SQLite database path, default `web/users.db`.
- `JWT_SECRET` - required and must be strong in production.
- `JWT_EXPIRES_IN` - JWT lifetime, e.g. `7d`, `12h`, `30m`.
- `CORS_ORIGIN` - comma-separated allowed origins. Development defaults to localhost origins.
- `BCRYPT_ROUNDS` - password hashing cost, default 10 (4 in tests).
- `AUTH_RATE_LIMIT_WINDOW_MS` / `AUTH_RATE_LIMIT_MAX` - auth rate limiting settings.
- `JSON_BODY_LIMIT` - request size guard.
- `GAME_WORLDS_DIR` - directory for published world files.
- `GAME_SERVER_BASE_PORT` - first port to try for game instances, default `7777`.
- `SERVER_EXECUTABLE_PATH` / `CLIENT_EXECUTABLE_PATH` - local executables the web server launches.
- `PUBLIC_BASE_URL` - URL passed to native server/client for web auth, default `http://localhost:3000`.

Production startup fails if `JWT_SECRET` is missing, too short, or left as an unsafe placeholder.

## Web API

Authentication:

- `POST /api/signup` with JSON `{ "username": "Player_1", "password": "long-password" }`
- `POST /api/login` with JSON `{ "username": "Player_1", "password": "long-password" }`
- `POST /api/verify` with `Authorization: Bearer <token>`

Avatar:

- `GET /api/avatar` with `Authorization: Bearer <token>`
- `POST /api/avatar` with `Authorization: Bearer <token>` and JSON `{ "avatar": { ...six RGB arrays..., "faceId": "wink" } }`

Valid avatar `faceId` values are `classic`, `happy`, `surprised`, `smirk`, and `wink`. The web previews and native Client use the same transparent face PNG assets.

Profiles and friends:

- `GET /api/me/social` returns the signed-in profile, friends, and friend requests.
- `POST /api/me/playtime` with `{ "seconds": 120 }` records authenticated playtime.
- `GET /api/users/search?q=Player` searches users by username or exact user ID.
- `GET /api/users/:id` returns a public profile, avatar, stats, and friends.
- `POST /api/friends/request` with `{ "userId": 2 }` sends or accepts a friend request.
- `POST /api/friends/respond` with `{ "userId": 2, "action": "accept" }` accepts or declines an incoming request.
- `POST /api/friends/remove` with `{ "userId": 2 }` removes a friend or cancels a pending request.

Games:

- `GET /api/games` lists public games for the games page.
- `GET /api/games/:id` returns game details, world stats, and active local instances.
- `GET /api/games/mine` with `Authorization: Bearer <token>` lists your published games.
- `POST /api/games` with `Authorization: Bearer <token>` and `{ "title": "...", "description": "...", "isPublic": true, "worldText": "..." }` publishes a world file.
- `POST /api/games/:id/play` with `Authorization: Bearer <token>` starts or reuses a local game server and returns native Player launch metadata.

Tokens are no longer accepted in avatar URLs. `/api/verify` still accepts a body token as a deprecated compatibility fallback, but new callers should use the Authorization header.

The game server reports authenticated player sessions to `/api/me/playtime` while a player is connected and once more when the player disconnects.

## Studio Controls

- Right mouse drag: look around.
- `W/A/S/D`: move camera.
- `E/Q`: move camera up/down.
- Shift: faster camera movement.
- Left click: select part.
- Toolbox: add parts, switch move/rotate/scale tools, enable snap.
- Properties: edit selected part fields.
- Publish: sign in against the web server, save the current world, and publish it to the site as a public or private game.

## Multi-Game Local Hosting

The website stores published `.world` files under `web/game-worlds` by default. When a signed-in user presses Play on a game page, the web backend finds an open local port, starts `Server.exe --port <port> --world <published-world> --web <site-url>`, and can launch `Client.exe --server <host> --port <port> --web <site-url> --token <jwt> --connect` when `CLIENT_EXECUTABLE_PATH` points to a built Player.

Useful launch environment variables:

- `GAME_WORLDS_DIR` - directory for published world files.
- `GAME_SERVER_BASE_PORT` - first port to try for game instances, default `7777`.
- `SERVER_EXECUTABLE_PATH` - local game server executable.
- `CLIENT_EXECUTABLE_PATH` - local Player executable.
- `PUBLIC_BASE_URL` - URL passed to native server/client for web auth, default `http://localhost:3000`.

Scale operations assign the decomposed absolute scale from ImGuizmo and clamp it to a safe positive range. They do not multiply scale every frame.

## Known Limitations

- This is a prototype and should be treated as local-only.
- Client and Server are Windows-only until socket and HTTP abstractions are made portable. The web backend is cross-platform.
- The binary protocol still depends on matching C++ struct layouts across the same build family.
- The web service uses SQLite and local JWT storage in the browser; it is not hardened for internet deployment.
- Client UI/networking code remains large and should be split further.
- C++ build/test is verified with CMake, Ninja, and MinGW GCC on Windows plus GCC on Linux. Client, Server, and Studio runtime behavior still need manual testing with a graphics-capable Windows desktop.

## Troubleshooting

- `cmake is not recognized`: install CMake or add it to PATH.
- `cl not found`: run from a Visual Studio Developer shell or install the C++ workload.
- `JWT_SECRET must be set`: set a strong secret in production or use development mode locally.
- Client login fails: start the C++ web server first and confirm `http://localhost:3000` is reachable.
- Client cannot connect: start `Server.exe`, allow firewall access, and connect to `127.0.0.1` for local testing.
