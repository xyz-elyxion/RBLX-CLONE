#pragma once

// Shared inline helpers for the Limey C++ web server translation units.

#include "HttpServer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace web {
namespace util {

constexpr size_t kMaxWorldBytes = 2 * 1024 * 1024;
inline const char* kDefaultGameIcon = "/assets/games/default-game-icon.png";
inline const char* kDefaultGameBanner = "/assets/games/default-game-banner.png";
inline const char* kFaceIds[] = {"classic", "happy", "surprised", "smirk", "wink"};

inline bool isFaceId(const std::string& value) {
    for (const char* id : kFaceIds) {
        if (value == id) return true;
    }
    return false;
}

inline std::string nowIso() {
    const std::time_t now = std::time(nullptr);
    char buffer[32];
    std::tm tmValue{};
#if defined(_WIN32)
    gmtime_s(&tmValue, &now);
#else
    gmtime_r(&now, &tmValue);
#endif
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", &tmValue);
    return buffer;
}

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline int64_t parseIsoMs(const std::string& iso) {
    if (iso.empty()) return 0;
    std::tm tmValue{};
    int millis = 0;
#if defined(_WIN32)
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d.%d", &tmValue.tm_year, &tmValue.tm_mon, &tmValue.tm_mday,
                 &tmValue.tm_hour, &tmValue.tm_min, &tmValue.tm_sec, &millis) >= 6) {
#else
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d.%d", &tmValue.tm_year, &tmValue.tm_mon, &tmValue.tm_mday,
               &tmValue.tm_hour, &tmValue.tm_min, &tmValue.tm_sec, &millis) >= 6) {
#endif
        tmValue.tm_year -= 1900;
        tmValue.tm_mon -= 1;
#if defined(_WIN32)
        const time_t value = _mkgmtime(&tmValue);
#else
        const time_t value = timegm(&tmValue);
#endif
        if (value >= 0) return static_cast<int64_t>(value) * 1000 + millis;
    }
    return nowMs();
}

inline std::string trim(const std::string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

inline std::string lower(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string collapseWhitespace(const std::string& value) {
    std::string out;
    bool inSpace = false;
    for (char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            inSpace = true;
            continue;
        }
        if (inSpace && !out.empty()) out += ' ';
        inSpace = false;
        out += c;
    }
    return out;
}

inline int64_t parsePositiveInt(const std::string& value) {
    if (value.empty()) return 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') return 0;
    return parsed > 0 ? parsed : 0;
}

inline bool toBool(const json& value, bool fallback) {
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_integer()) return value.get<int64_t>() != 0;
    if (value.is_string()) {
        const std::string normalized = lower(trim(value.get<std::string>()));
        if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "public") return true;
        if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "private") return false;
    }
    return fallback;
}

inline std::string slugify(const std::string& input) {
    std::string slug;
    bool pendingDash = false;
    for (unsigned char c : lower(trim(input))) {
        if (std::isalnum(c)) {
            if (pendingDash && !slug.empty()) slug += '-';
            pendingDash = false;
            slug += static_cast<char>(c);
        } else {
            pendingDash = true;
        }
    }
    if (slug.size() > 48) slug.resize(48);
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug.empty() ? "game" : slug;
}

inline std::string joinPath(const std::string& base, const std::string& child) {
    return (std::filesystem::path(base) / child).string();
}

inline bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

inline bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

inline bool writeFileAtomic(const std::string& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << content;
    return true;
}

inline bool ensureDirectory(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec || std::filesystem::is_directory(path);
}

inline HttpResponse jsonResponse(int status, const json& body) {
    HttpResponse response;
    response.status = status;
    response.set("Content-Type", "application/json; charset=utf-8");
    response.body = body.dump();
    return response;
}

inline HttpResponse errorResponse(int status, const std::string& message) {
    return jsonResponse(status, json{{"error", message}});
}

inline HttpResponse notFound() {
    return errorResponse(404, "Not found.");
}

struct QueryParams {
    std::vector<std::pair<std::string, std::string>> pairs;

    std::string get(const std::string& name) const {
        for (const auto& pair : pairs) {
            if (pair.first == name) return pair.second;
        }
        return "";
    }
};

inline QueryParams parseQuery(const std::string& query) {
    QueryParams params;
    size_t start = 0;
    while (start <= query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        if (end > start) {
            const std::string pairText = query.substr(start, end - start);
            const size_t equals = pairText.find('=');
            if (equals != std::string::npos) {
                params.pairs.emplace_back(pairText.substr(0, equals), pairText.substr(equals + 1));
            } else {
                params.pairs.emplace_back(pairText, "");
            }
        }
        if (end >= query.size()) break;
        start = end + 1;
    }
    return params;
}

} // namespace util
} // namespace web
