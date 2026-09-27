#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace web {

// SHA-256 (FIPS 180-4).
std::string sha256Hex(const std::string& data);
std::vector<uint8_t> sha256Raw(const std::string& data);

// HMAC-SHA256 (RFC 2104).
std::vector<uint8_t> hmacSha256Raw(const std::string& key, const std::string& data);

// Base64 (RFC 4648) with URL-safe alphabet and no padding, as used by JWT.
std::string base64UrlEncode(const std::string& data);
std::string base64UrlDecode(const std::string& data);

// HS256 JWT signing/verification (self-contained, no OpenSSL).
// Tokens carry {"alg":"HS256","typ":"JWT"} plus sub/userId/username claims,
// matching the tokens the previous Node server issued with jsonwebtoken.
std::string jwtSign(const std::string& secret, int64_t userId, const std::string& username,
                    int64_t expiresInSeconds);
int jwtVerify(const std::string& token, const std::string& secret, int64_t& userId, std::string& username);

// bcrypt ($2a/$2b/$2y$...) hashing compatible with the Node bcrypt package's
// existing user database. rounds is the cost factor (e.g. 10).
std::string bcryptHash(const std::string& password, int rounds);
bool bcryptVerify(const std::string& password, const std::string& hash);

// Parses "7d"-style durations from the previous Node server config.
int64_t parseJwtExpiresInSeconds(const std::string& value, int64_t fallbackSeconds);

} // namespace web
