# ---- Stage 1: build (compiles WebServer and runs its test suite) ----
FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ gcc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY vendor/ vendor/
COPY src/web/ src/web/
COPY tests/ tests/
# The test suite serves the real site and seeds starter games, so it needs
# the same runtime assets the server ships with.
COPY web/public web/public
COPY web/seed-worlds web/seed-worlds
COPY ServerWorld.world .

# Same direct-compile recipe CI uses for the web stack (no GL/CMake needed).
RUN set -eux; \
    mkdir -p build; \
    gcc -std=c11 -O2 -c vendor/sqlite/sqlite3.c -Ivendor/sqlite -DSQLITE_THREADSAFE=1 -o build/sqlite3.o; \
    gcc -std=c99 -c vendor/bcrypt/crypt_blowfish.c -Ivendor/bcrypt -o build/crypt_blowfish.o; \
    gcc -std=c99 -c vendor/bcrypt/crypt_gensalt.c -Ivendor/bcrypt -o build/crypt_gensalt.o; \
    for f in WebCrypto HttpServer WebApp WebAppApi WebAppSocial WebAppGames WebAppWorld WebAppInstances; do \
      g++ -std=c++17 -O2 -c "src/web/$f.cpp" -Isrc/web -Ivendor/json -Ivendor/sqlite -Ivendor/bcrypt -o "build/$f.o"; \
    done; \
    g++ -std=c++17 -O2 -c src/web/WebServerMain.cpp -Isrc/web -Ivendor/json -Ivendor/sqlite -Ivendor/bcrypt -o build/WebServerMain.o; \
    g++ -std=c++17 -O2 -c tests/web_server_tests.cpp -Isrc/web -Ivendor/json -Ivendor/sqlite -Ivendor/bcrypt -o build/web_tests.o; \
    g++ -o build/WebServer \
        build/WebServerMain.o build/WebCrypto.o build/HttpServer.o \
        build/WebApp.o build/WebAppApi.o build/WebAppSocial.o build/WebAppGames.o \
        build/WebAppWorld.o build/WebAppInstances.o \
        build/sqlite3.o build/crypt_blowfish.o build/crypt_gensalt.o -lpthread; \
    g++ -o build/web_server_tests \
        build/web_tests.o build/WebCrypto.o build/HttpServer.o \
        build/WebApp.o build/WebAppApi.o build/WebAppSocial.o build/WebAppGames.o \
        build/WebAppWorld.o build/WebAppInstances.o \
        build/sqlite3.o build/crypt_blowfish.o build/crypt_gensalt.o -lpthread

# Fail the image build if the server's own test suite fails.
# Tests use repo-root-relative paths, so run from /src.
RUN cd /src && ./build/web_server_tests

# ---- Stage 2: runtime ----
FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends wget \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 limey \
    && mkdir -p /data \
    && chown limey:limey /data

WORKDIR /app

COPY --from=build /src/build/WebServer /app/WebServer
COPY web/public /app/web/public
COPY web/seed-worlds /app/web/seed-worlds
COPY ServerWorld.world /app/ServerWorld.world

# JWT_SECRET must be provided at run time (see README). DATABASE_PATH and
# GAME_WORLDS_DIR point at the persistent volume so accounts and published
# games survive container replacement.
ENV NODE_ENV=production \
    PORT=3000 \
    DATABASE_PATH=/data/users.db \
    GAME_WORLDS_DIR=/data/game-worlds

USER limey
EXPOSE 3000
VOLUME ["/data"]

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD wget -qO- "http://127.0.0.1:${PORT}/" >/dev/null 2>&1 || exit 1

ENTRYPOINT ["/app/WebServer"]
