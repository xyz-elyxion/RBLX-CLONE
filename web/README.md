# Limey Web Backend (native C++)

This is the native C++ web backend for account signup/login, token verification, avatar storage, and the public game catalog. It replaces the previous Node.js/Express service with the same HTTP API, the same SQLite database (`users.db`), and the same environment variables — no Node.js required.

## Build

```powershell
cmake -S . -B build/web -DRBLX_BUILD_CLIENT=OFF -DRBLX_BUILD_SERVER=OFF -DRBLX_BUILD_STUDIO=OFF
cmake --build build/web --target WebServer
```

## Run

```powershell
.\build\web\WebServer.exe --port 3000
```

It serves the site from `public/` and the API under `/api/`. Copy `.env.example` to `.env` for local overrides. Do not commit `.env`.

## Docker

A multi-stage `Dockerfile` at the repo root builds a production image: stage 1 compiles the server and runs its test suite (the image build fails if tests fail), stage 2 is a minimal Debian runtime with the site assets baked in.

```bash
docker build -t limey-webserver .
docker run --rm -p 3000:3000 \
  -e JWT_SECRET="replace-with-a-long-random-secret" \
  -v limey-data:/data \
  limey-webserver
```

- Listens on `0.0.0.0:3000` (override with `PORT`).
- `NODE_ENV=production` is set in the image; the server refuses to start without `JWT_SECRET`.
- State lives on the `/data` volume: `DATABASE_PATH=/data/users.db`, `GAME_WORLDS_DIR=/data/game-worlds` (both overridable).
- Starter games seed on first boot from the baked-in `web/seed-worlds` and `ServerWorld.world`.
- The container includes only the web backend — game-server instance spawning (`/api/games/:id/play`) assumes `Server.exe`/`Client.exe` exist on the host, so it is only meaningful when running the exes natively.

## Environment

- `NODE_ENV` - `development`, `test`, or `production`.
- `PORT` - server port, default `3000`.
- `DATABASE_PATH` - SQLite database path, default `web/users.db`.
- `JWT_SECRET` - required and strong in production.
- `JWT_EXPIRES_IN` - token lifetime, e.g. `7d`, `12h`, `30m`.
- `CORS_ORIGIN` - comma-separated allowed origins.
- `BCRYPT_ROUNDS` - password hashing cost, default 10 (4 in test mode).
- `AUTH_RATE_LIMIT_WINDOW_MS` and `AUTH_RATE_LIMIT_MAX` - auth endpoint rate limit.
- `GAME_WORLDS_DIR` - directory for published `.world` files.
- `GAME_SERVER_HOST` and `GAME_SERVER_BASE_PORT` - local host/port range for spawned game instances.
- `GAME_INSTANCE_EMPTY_GRACE_MS` - how long an empty spawned game instance stays alive before the web server stops it, default `30000`.
- `SERVER_EXECUTABLE_PATH` - `Server.exe` path used when a game page starts an instance.
- `CLIENT_EXECUTABLE_PATH` - `Client.exe` path used when the Play button launches the native player.
- `PUBLIC_BASE_URL` - site URL passed to native server/client for web auth.

## API

- `POST /api/signup` with `{ "username": "Player_1", "password": "long-password" }`.
- `POST /api/login` with `{ "username": "Player_1", "password": "long-password" }`.
- `POST /api/verify` with `Authorization: Bearer <token>`.
- `GET /api/avatar` with `Authorization: Bearer <token>`.
- `POST /api/avatar` with `Authorization: Bearer <token>` and `{ "avatar": { ...six RGB arrays..., "faceId": "wink" } }`.
- `GET /api/me/social` with `Authorization: Bearer <token>`.
- `POST /api/me/playtime` with `Authorization: Bearer <token>` and `{ "seconds": 120 }`.
- `GET /api/users/search?q=Player` with `Authorization: Bearer <token>`.
- `GET /api/users/:id` with `Authorization: Bearer <token>`.
- `POST /api/friends/request` with `Authorization: Bearer <token>`.
- `POST /api/friends/respond` with `Authorization: Bearer <token>`.
- `POST /api/friends/remove` with `Authorization: Bearer <token>`.
- `GET /api/games` lists public games.
- `GET /api/games/:id` returns game details, world stats, and active local instances.
- `GET /api/games/mine` with `Authorization: Bearer <token>` lists your games.
- `POST /api/games` with `Authorization: Bearer <token>` publishes a world file as a game.
- `POST /api/games/publish` creates or updates a game for Studio publishing.
- `POST /api/games/:id/play` starts or reuses a local game instance on demand and returns Player launch metadata.
- `POST /api/game-instances/:id/heartbeat` is used by the native game server to report active player counts.

Avatar tokens are not accepted in query strings. `/api/verify` has a deprecated body-token fallback for older callers.

Valid avatar `faceId` values are `classic`, `happy`, `surprised`, `smirk`, and `wink`.

The Windows game server uses `/api/me/playtime` to add authenticated session time during play and on disconnect. It also sends instance heartbeats so the website can stop empty game processes and start them again the next time someone presses Play.

The games page lives at `/games`, game detail pages live at `/games/:id`, and the browser publishing form lives at `/create`.

## Dependencies

All third-party code is vendored under `vendor/` and compiled from source:

- `vendor/sqlite` - SQLite amalgamation (public domain).
- `vendor/json` - nlohmann/json (MIT).
- `vendor/bcrypt` - Openwall crypt_blowfish (public domain), used for bcrypt password hashes compatible with the original Node `bcrypt` package.

There is no package manager, runtime, or interpreter involved.
