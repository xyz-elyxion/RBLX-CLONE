#include "WebCrypto.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

extern "C" {
// Openwall crypt_blowfish, public domain. The vendored header has no extern "C"
// guard, so it is included here inside C linkage.
#include "crypt_blowfish.h"
}

namespace web {

namespace {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), single-file implementation.
// ---------------------------------------------------------------------------

struct Sha256Context {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t buffer[64] = {0};
    uint64_t bitLength = 0;
    size_t bufferLength = 0;
};

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t rotr(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

void sha256Block(Sha256Context& ctx, const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
    uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g; g = f; f = e;
        e = d + temp1;
        d = c; c = b; b = a;
        a = temp1 + temp2;
    }

    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

void sha256Update(Sha256Context& ctx, const uint8_t* data, size_t length) {
    ctx.bitLength += static_cast<uint64_t>(length) * 8;
    while (length > 0) {
        const size_t take = std::min(length, static_cast<size_t>(64) - ctx.bufferLength);
        std::memcpy(ctx.buffer + ctx.bufferLength, data, take);
        ctx.bufferLength += take;
        data += take;
        length -= take;
        if (ctx.bufferLength == 64) {
            sha256Block(ctx, ctx.buffer);
            ctx.bufferLength = 0;
        }
    }
}

void sha256Final(Sha256Context& ctx, uint8_t out[32]) {
    const uint64_t bitLength = ctx.bitLength;
    const uint8_t pad = 0x80;
    sha256Update(ctx, &pad, 1);
    const uint8_t zero = 0;
    while (ctx.bufferLength != 56) {
        sha256Update(ctx, &zero, 1);
    }
    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) {
        lengthBytes[i] = static_cast<uint8_t>(bitLength >> (56 - i * 8));
    }
    sha256Update(ctx, lengthBytes, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(ctx.state[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i]);
    }
}

const char kBase64UrlChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

} // namespace

std::vector<uint8_t> sha256Raw(const std::string& data) {
    Sha256Context ctx;
    sha256Update(ctx, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    uint8_t digest[32];
    sha256Final(ctx, digest);
    return std::vector<uint8_t>(digest, digest + 32);
}

std::string sha256Hex(const std::string& data) {
    const std::vector<uint8_t> digest = sha256Raw(data);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 0x0f];
    }
    return out;
}

std::vector<uint8_t> hmacSha256Raw(const std::string& key, const std::string& data) {
    std::string block = key;
    if (block.size() < 64) block.resize(64, '\0');

    std::string innerKey(64, '\0');
    std::string outerKey(64, '\0');
    for (size_t i = 0; i < 64; ++i) {
        const char k = i < key.size() ? key[i] : '\0';
        innerKey[i] = static_cast<char>(k ^ 0x36);
        outerKey[i] = static_cast<char>(k ^ 0x5c);
    }

    Sha256Context inner;
    sha256Update(inner, reinterpret_cast<const uint8_t*>(innerKey.data()), 64);
    sha256Update(inner, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    uint8_t innerDigest[32];
    sha256Final(inner, innerDigest);

    Sha256Context outer;
    sha256Update(outer, reinterpret_cast<const uint8_t*>(outerKey.data()), 64);
    sha256Update(outer, innerDigest, 32);
    uint8_t outerDigest[32];
    sha256Final(outer, outerDigest);
    return std::vector<uint8_t>(outerDigest, outerDigest + 32);
}

std::string base64UrlEncode(const std::string& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        const uint32_t value = (static_cast<uint8_t>(data[i]) << 16) |
                               (static_cast<uint8_t>(data[i + 1]) << 8) |
                               static_cast<uint8_t>(data[i + 2]);
        out += kBase64UrlChars[(value >> 18) & 0x3f];
        out += kBase64UrlChars[(value >> 12) & 0x3f];
        out += kBase64UrlChars[(value >> 6) & 0x3f];
        out += kBase64UrlChars[value & 0x3f];
        i += 3;
    }
    const size_t remaining = data.size() - i;
    if (remaining == 1) {
        const uint32_t value = static_cast<uint8_t>(data[i]) << 16;
        out += kBase64UrlChars[(value >> 18) & 0x3f];
        out += kBase64UrlChars[(value >> 12) & 0x3f];
    } else if (remaining == 2) {
        const uint32_t value = (static_cast<uint8_t>(data[i]) << 16) |
                               (static_cast<uint8_t>(data[i + 1]) << 8);
        out += kBase64UrlChars[(value >> 18) & 0x3f];
        out += kBase64UrlChars[(value >> 12) & 0x3f];
        out += kBase64UrlChars[(value >> 6) & 0x3f];
    }
    return out;
}

std::string base64UrlDecode(const std::string& input) {
    auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    std::string out;
    uint32_t accumulator = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int value = valueOf(c);
        if (value < 0) return out;
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((accumulator >> bits) & 0xff);
        }
    }
    return out;
}

namespace {

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trim(const std::string& value) {
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

} // namespace

std::string jwtSign(const std::string& secret, int64_t userId, const std::string& username,
                    int64_t expiresInSeconds) {
    const int64_t issuedAt = nowSeconds();
    std::ostringstream sub;
    sub << userId;

    // {"alg":"HS256","typ":"JWT"}
    const std::string header = base64UrlEncode(R"({"alg":"HS256","typ":"JWT"})");
    std::ostringstream payloadJson;
    payloadJson << "{\"sub\":\"" << sub.str() << "\",\"userId\":" << userId
                << ",\"username\":\"" << username << "\",\"iat\":" << issuedAt
                << ",\"exp\":" << (issuedAt + expiresInSeconds) << "}";
    const std::string payload = base64UrlEncode(payloadJson.str());
    const std::string signingInput = header + "." + payload;
    const std::vector<uint8_t> signature = hmacSha256Raw(secret, signingInput);
    return signingInput + "." + base64UrlEncode(std::string(signature.begin(), signature.end()));
}

int jwtVerify(const std::string& token, const std::string& secret, int64_t& userId, std::string& username) {
    const size_t firstDot = token.find('.');
    const size_t secondDot = firstDot == std::string::npos ? std::string::npos : token.find('.', firstDot + 1);
    if (firstDot == std::string::npos || secondDot == std::string::npos) return 1;

    const std::string signingInput = token.substr(0, secondDot);
    const std::string providedSignature = base64UrlDecode(token.substr(secondDot + 1));
    const std::vector<uint8_t> expected = hmacSha256Raw(secret, signingInput);
    if (providedSignature.size() != expected.size()) return 2;
    if (std::memcmp(providedSignature.data(), expected.data(), expected.size()) != 0) return 2;

    const std::string payloadJson = base64UrlDecode(token.substr(firstDot + 1, secondDot - firstDot - 1));
    if (payloadJson.find("\"exp\":") == std::string::npos) return 3;

    const size_t expPos = payloadJson.find("\"exp\":");
    const size_t expValueStart = payloadJson.find_first_of("0123456789", expPos + 6);
    if (expValueStart == std::string::npos) return 3;
    const int64_t exp = std::strtoll(payloadJson.c_str() + expValueStart, nullptr, 10);
    if (exp <= nowSeconds()) return 3;

    const size_t userIdPos = payloadJson.find("\"userId\":");
    if (userIdPos == std::string::npos) return 4;
    const size_t userIdStart = payloadJson.find_first_of("0123456789", userIdPos + 9);
    if (userIdStart == std::string::npos) return 4;
    userId = std::strtoll(payloadJson.c_str() + userIdStart, nullptr, 10);
    if (userId <= 0) return 4;

    const size_t usernamePos = payloadJson.find("\"username\":\"");
    if (usernamePos != std::string::npos) {
        const size_t nameStart = usernamePos + 12;
        const size_t nameEnd = payloadJson.find('"', nameStart);
        if (nameEnd != std::string::npos) {
            username = payloadJson.substr(nameStart, nameEnd - nameStart);
        }
    }
    return 0;
}

std::string bcryptHash(const std::string& password, int rounds) {
    // bcrypt only uses the first 72 bytes; mirroring the Node bcrypt package.
    std::string key = password.substr(0, 72);

    uint8_t seed[16];
    {
        static std::mutex seedMutex;
        static uint64_t counter = 0;
        std::lock_guard<std::mutex> lock(seedMutex);
        const std::string digest = sha256Hex(key + "|" + std::to_string(nowSeconds()) + "|" +
                                             std::to_string(++counter) + "|" +
                                             std::to_string(std::chrono::high_resolution_clock::now()
                                                                .time_since_epoch()
                                                                .count()));
        for (int i = 0; i < 16; ++i) {
            seed[i] = static_cast<uint8_t>(std::stoul(digest.substr(i * 2, 2), nullptr, 16));
        }
    }

    char salt[64];
    if (!_crypt_gensalt_blowfish_rn("$2a$", static_cast<unsigned long>(rounds),
                                    reinterpret_cast<const char*>(seed), 16, salt, sizeof(salt))) {
        return "";
    }
    char output[128];
    if (!_crypt_blowfish_rn(key.c_str(), salt, output, sizeof(output))) {
        return "";
    }
    return std::string(output);
}

bool bcryptVerify(const std::string& password, const std::string& hash) {
    if (hash.size() < 59 || (hash.compare(0, 4, "$2a$") != 0 && hash.compare(0, 4, "$2b$") != 0 &&
                             hash.compare(0, 4, "$2y$") != 0)) {
        return false;
    }
    std::string key = password.substr(0, 72);
    char output[128];
    if (!_crypt_blowfish_rn(key.c_str(), hash.c_str(), output, sizeof(output))) {
        return false;
    }
    return std::string(output) == hash;
}

int64_t parseJwtExpiresInSeconds(const std::string& value, int64_t fallbackSeconds) {
    const std::string trimmed = trim(value);
    if (trimmed.empty()) return fallbackSeconds;

    char* end = nullptr;
    const long long number = std::strtoll(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str()) return fallbackSeconds;

    const std::string unit = trim(end);
    int64_t multiplier = 1;
    if (unit.empty() || unit == "s") multiplier = 1;
    else if (unit == "m") multiplier = 60;
    else if (unit == "h") multiplier = 3600;
    else if (unit == "d") multiplier = 86400;
    else return fallbackSeconds;

    return number > 0 ? number * multiplier : fallbackSeconds;
}

} // namespace web
