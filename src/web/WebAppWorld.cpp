#include "WebApp.h"
#include "WebUtil.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace web {

namespace {

// Tokenizer with the same quoting rules as web/server.js: double-quoted
// segments may contain escaped quotes; otherwise whitespace-separated.
std::vector<std::string> tokenizeWorldLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;
    bool hasToken = false;

    auto pushToken = [&]() {
        if (hasToken) {
            tokens.push_back(current);
            current.clear();
            hasToken = false;
        }
    };

    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '\\' && i + 1 < line.size() && line[i + 1] == '"') {
                current += '"';
                ++i;
            } else if (c == '"') {
                inQuotes = false;
            } else {
                current += c;
            }
        } else if (c == '"') {
            pushToken();
            inQuotes = true;
            hasToken = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            pushToken();
        } else {
            current += c;
            hasToken = true;
        }
    }
    pushToken();
    return tokens;
}

bool worldBool(const std::string& value) {
    return value == "1" || value == "true";
}

} // namespace

bool WebApp::analyzeWorldSource(const std::string& worldText, WorldAnalysis& analysis,
                                std::string& errorMessage) const {
    if (worldText.empty() || util::trim(worldText).empty()) {
        errorMessage = "World data is required.";
        return false;
    }

    if (worldText.size() > util::kMaxWorldBytes) {
        errorMessage = "World data is too large for this local server.";
        return false;
    }

    // Normalize line endings to '\n'.
    std::string normalized;
    normalized.reserve(worldText.size() + 1);
    for (size_t i = 0; i < worldText.size(); ++i) {
        if (worldText[i] == '\r') {
            if (i + 1 < worldText.size() && worldText[i + 1] == '\n') ++i;
            normalized += '\n';
        } else {
            normalized += worldText[i];
        }
    }

    std::vector<std::string> lines;
    {
        std::string line;
        for (char c : normalized) {
            if (c == '\n') {
                if (!util::trim(line).empty()) lines.push_back(line);
                line.clear();
            } else {
                line += c;
            }
        }
        if (!util::trim(line).empty()) lines.push_back(line);
    }

    if (lines.empty()) {
        errorMessage = "World file header must contain a valid part count.";
        return false;
    }

    const int64_t partsCount = util::parsePositiveInt(util::trim(lines[0]));
    if (partsCount < 0 || partsCount > 4096) {
        errorMessage = "World file header must contain a valid part count.";
        return false;
    }

    if (static_cast<int64_t>(lines.size()) - 1 < partsCount) {
        errorMessage = "World file ended before all parts were present.";
        return false;
    }

    int dynamicPartsCount = 0;
    int spawnCount = 0;
    const size_t numberFieldIndexes[] = {2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 22};

    for (int64_t index = 0; index < partsCount; ++index) {
        const std::vector<std::string> tokens = tokenizeWorldLine(lines[static_cast<size_t>(index) + 1]);
        if (tokens.size() < 23) {
            errorMessage = "World part " + std::to_string(index + 1) + " is missing fields.";
            return false;
        }

        char* end = nullptr;
        const long shape = std::strtol(tokens[0].c_str(), &end, 10);
        if (end == tokens[0].c_str() || *end != '\0' || shape < 0 || shape > 3) {
            errorMessage = "World part " + std::to_string(index + 1) + " has an unsupported shape.";
            return false;
        }

        for (size_t fieldIndex : numberFieldIndexes) {
            const std::string& token = tokens[fieldIndex];
            char* numberEnd = nullptr;
            std::strtod(token.c_str(), &numberEnd);
            if (numberEnd == token.c_str() || *numberEnd != '\0') {
                errorMessage = "World part " + std::to_string(index + 1) + " contains invalid numeric data.";
                return false;
            }
        }

        if (worldBool(tokens[5])) ++spawnCount;
        if (!worldBool(tokens[20]) && !worldBool(tokens[3])) ++dynamicPartsCount;
    }

    analysis.source = normalized;
    if (!normalized.empty() && normalized.back() != '\n') analysis.source += '\n';
    analysis.partsCount = static_cast<int>(partsCount);
    analysis.dynamicPartsCount = dynamicPartsCount;
    analysis.spawnCount = spawnCount;
    return true;
}

} // namespace web
