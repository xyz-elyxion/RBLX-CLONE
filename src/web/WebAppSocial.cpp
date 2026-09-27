#include "WebApp.h"
#include "Db.h"
#include "WebCrypto.h"
#include "WebUtil.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace web {

namespace {

json avatarJsonFromColumns(Statement& row, bool present) {
    if (!present) {
        return json{
            {"headColor", json::array({0.8, 0.6, 0.4})},
            {"torsoColor", json::array({0.2, 0.4, 0.8})},
            {"leftArmColor", json::array({0.8, 0.6, 0.4})},
            {"rightArmColor", json::array({0.8, 0.6, 0.4})},
            {"leftLegColor", json::array({0.2, 0.6, 0.2})},
            {"rightLegColor", json::array({0.2, 0.6, 0.2})},
            {"faceId", "classic"}
        };
    }
    auto color = [&row](int r, int g, int b) {
        return json::array({row.getDouble(r), row.getDouble(g), row.getDouble(b)});
    };
    return json{
        {"headColor", color(7, 8, 9)},
        {"torsoColor", color(10, 11, 12)},
        {"leftArmColor", color(13, 14, 15)},
        {"rightArmColor", color(16, 17, 18)},
        {"leftLegColor", color(19, 20, 21)},
        {"rightLegColor", color(22, 23, 24)},
        {"faceId", row.isNull(6) || !util::isFaceId(row.getText(6)) ? "classic" : row.getText(6)}
    };
}

// Column order must match kPublicUserColumns in the queries below:
// 0 u.id, 1 u.username, 2 u.created_at, 3 playtime_seconds, 4 last_played_at,
// 5 a.user_id, 6 a.face_id, 7..9 head, 10..12 torso, 13..15 leftArm,
// 16..18 rightArm, 19..21 leftLeg, 22..24 rightLeg
const char* kPublicUserColumns =
    "u.id, u.username, u.created_at, "
    "COALESCE(s.playtime_seconds, 0) AS playtime_seconds, "
    "s.last_played_at, "
    "a.user_id, a.face_id, "
    "a.head_color_r, a.head_color_g, a.head_color_b, "
    "a.torso_color_r, a.torso_color_g, a.torso_color_b, "
    "a.left_arm_color_r, a.left_arm_color_g, a.left_arm_color_b, "
    "a.right_arm_color_r, a.right_arm_color_g, a.right_arm_color_b, "
    "a.left_leg_color_r, a.left_leg_color_g, a.left_leg_color_b, "
    "a.right_leg_color_r, a.right_leg_color_g, a.right_leg_color_b";

json publicUserFromRow(Statement& row, const std::string& relationship, int64_t friendCount) {
    json user;
    user["id"] = row.getInt(0);
    user["username"] = row.getText(1);
    user["createdAt"] = row.isNull(2) ? json(nullptr) : json(row.getText(2));
    user["avatar"] = avatarJsonFromColumns(row, !row.isNull(5));
    user["stats"] = json{
        {"playtimeSeconds", row.getInt(3)},
        {"lastPlayedAt", row.isNull(4) ? json(nullptr) : json(row.getText(4))}
    };
    user["friendCount"] = friendCount;
    user["relationship"] = relationship;
    return user;
}

int64_t friendCountFor(sqlite3* db, int64_t userId) {
    Statement stmt(db,
                   "SELECT COUNT(*) FROM friendships WHERE status = 'accepted' "
                   "AND (requester_id = ? OR addressee_id = ?)");
    stmt.bind(1, userId);
    stmt.bind(2, userId);
    return stmt.ok() && stmt.step() ? stmt.getInt(0) : 0;
}

struct FriendshipInfo {
    bool found = false;
    std::string status;
    int64_t requesterId = 0;
    int64_t id = 0;
};

FriendshipInfo fetchFriendship(sqlite3* db, int64_t firstUserId, int64_t secondUserId) {
    Statement stmt(db, "SELECT id, requester_id, addressee_id, status FROM friendships "
                       "WHERE (requester_id = ? AND addressee_id = ?) "
                       "OR (requester_id = ? AND addressee_id = ?)");
    stmt.bind(1, firstUserId);
    stmt.bind(2, secondUserId);
    stmt.bind(3, secondUserId);
    stmt.bind(4, firstUserId);
    FriendshipInfo info;
    if (stmt.ok() && stmt.step()) {
        info.found = true;
        info.id = stmt.getInt(0);
        info.requesterId = stmt.getInt(1);
        info.status = stmt.getText(3);
    }
    return info;
}

std::string relationshipForViewer(int64_t viewerId, int64_t userId, const FriendshipInfo& friendship) {
    if (viewerId == userId) return "self";
    if (!friendship.found) return "none";
    if (friendship.status == "accepted") return "friends";
    return friendship.requesterId == viewerId ? "outgoing" : "incoming";
}

json serializePublicUser(sqlite3* db, Statement& row, int64_t viewerId) {
    const int64_t userId = row.getInt(0);
    const int64_t friendCount = friendCountFor(db, userId);
    const FriendshipInfo friendship =
        viewerId != 0 && viewerId != userId ? fetchFriendship(db, viewerId, userId) : FriendshipInfo{};
    return publicUserFromRow(row, relationshipForViewer(viewerId, userId, friendship), friendCount);
}

struct AvatarPayload {
    double head[3];
    double torso[3];
    double leftArm[3];
    double rightArm[3];
    double leftLeg[3];
    double rightLeg[3];
    std::string faceId;
};

bool parseAvatarPayload(const json& body, AvatarPayload& values, std::string& errorMessage) {
    const std::string kError = "Avatar data must contain six RGB arrays with values from 0 to 1.";
    if (!body.contains("avatar") || !body["avatar"].is_object()) {
        errorMessage = kError;
        return false;
    }
    const json& avatar = body["avatar"];

    struct Field {
        const char* name;
        double* target;
    };
    const Field fields[] = {
        {"headColor", values.head},       {"torsoColor", values.torso},     {"leftArmColor", values.leftArm},
        {"rightArmColor", values.rightArm}, {"leftLegColor", values.leftLeg}, {"rightLegColor", values.rightLeg}
    };

    for (const Field& field : fields) {
        if (!avatar.contains(field.name) || !avatar[field.name].is_array() || avatar[field.name].size() != 3) {
            errorMessage = kError;
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            const json& component = avatar[field.name][i];
            if (!component.is_number()) {
                errorMessage = kError;
                return false;
            }
            const double value = component.get<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
                errorMessage = kError;
                return false;
            }
            field.target[i] = value;
        }
    }

    if (avatar.contains("faceId") && !avatar["faceId"].is_null()) {
        if (!avatar["faceId"].is_string() || !util::isFaceId(avatar["faceId"].get<std::string>())) {
            errorMessage = kError;
            return false;
        }
        values.faceId = avatar["faceId"].get<std::string>();
    } else {
        values.faceId = "classic";
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Social endpoints
// ---------------------------------------------------------------------------

HttpResponse WebApp::meSocial(const HttpRequest& request, int64_t userId) {
    (void)request;
    json profile, friends, incoming, outgoing;

    {
        Statement stmt(db_, "SELECT u.id, u.username, u.created_at, "
                            "COALESCE(s.playtime_seconds, 0), s.last_played_at, "
                            "a.user_id, a.face_id, "
                            "a.head_color_r, a.head_color_g, a.head_color_b, "
                            "a.torso_color_r, a.torso_color_g, a.torso_color_b, "
                            "a.left_arm_color_r, a.left_arm_color_g, a.left_arm_color_b, "
                            "a.right_arm_color_r, a.right_arm_color_g, a.right_arm_color_b, "
                            "a.left_leg_color_r, a.left_leg_color_g, a.left_leg_color_b, "
                            "a.right_leg_color_r, a.right_leg_color_g, a.right_leg_color_b "
                            "FROM users u "
                            "LEFT JOIN avatars a ON a.user_id = u.id "
                            "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
        stmt.bind(1, userId);
        if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
        profile = serializePublicUser(db_, stmt, userId);
    }

    {
        friends = json::array();
        Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                               " FROM friendships f "
                               "JOIN users u ON u.id = CASE WHEN f.requester_id = ? THEN f.addressee_id "
                               "ELSE f.requester_id END "
                               "LEFT JOIN avatars a ON a.user_id = u.id "
                               "LEFT JOIN user_stats s ON s.user_id = u.id "
                               "WHERE f.status = 'accepted' AND (f.requester_id = ? OR f.addressee_id = ?) "
                               "ORDER BY lower(u.username) LIMIT 48");
        stmt.bind(1, userId);
        stmt.bind(2, userId);
        stmt.bind(3, userId);
        while (stmt.ok() && stmt.step()) {
            friends.push_back(serializePublicUser(db_, stmt, userId));
        }
    }

    auto fetchRequests = [&](const char* ownerColumn, const char* otherColumn) {
        json result = json::array();
        std::string sql = std::string("SELECT ") + kPublicUserColumns +
                          " FROM friendships f "
                          "JOIN users u ON u.id = f.";
        sql += otherColumn;
        sql += " LEFT JOIN avatars a ON a.user_id = u.id LEFT JOIN user_stats s ON s.user_id = u.id "
               "WHERE f.status = 'pending' AND f.";
        sql += ownerColumn;
        sql += " = ? ORDER BY f.created_at DESC LIMIT 24";
        Statement stmt(db_, sql.c_str());
        stmt.bind(1, userId);
        while (stmt.ok() && stmt.step()) {
            const int64_t otherUserId = stmt.getInt(0);
            result.push_back(publicUserFromRow(stmt, ownerColumn == std::string("addressee_id") ? "incoming" : "outgoing",
                                               friendCountFor(db_, otherUserId)));
        }
        return result;
    };
    incoming = fetchRequests("addressee_id", "requester_id");
    outgoing = fetchRequests("requester_id", "addressee_id");

    return util::jsonResponse(
        200, json{{"success", true}, {"profile", profile}, {"friends", friends},
                  {"incomingRequests", incoming}, {"outgoingRequests", outgoing}});
}

HttpResponse WebApp::mePlaytime(const HttpRequest& request, int64_t userId) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    int64_t seconds = 0;
    if (body.contains("seconds") && body["seconds"].is_number()) {
        const double raw = body["seconds"].get<double>();
        if (std::isfinite(raw) && raw > 0) {
            seconds = std::min<int64_t>(static_cast<int64_t>(raw), 24 * 60 * 60);
        }
    }
    if (seconds == 0) {
        return util::errorResponse(400, "Playtime seconds must be a positive number.");
    }

    {
        Statement stmt(db_, "INSERT INTO user_stats (user_id, playtime_seconds, last_played_at, updated_at) "
                            "VALUES (?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
                            "ON CONFLICT(user_id) DO UPDATE SET "
                            "playtime_seconds = playtime_seconds + excluded.playtime_seconds, "
                            "last_played_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP");
        stmt.bind(1, userId);
        stmt.bind(2, seconds);
        if (!stmt.ok() || !stmt.exec()) return util::errorResponse(500, "Server error.");
    }

    json profile;
    {
        Statement stmt(db_, "SELECT u.id, u.username, u.created_at, "
                            "COALESCE(s.playtime_seconds, 0), s.last_played_at, "
                            "a.user_id, a.face_id, "
                            "a.head_color_r, a.head_color_g, a.head_color_b, "
                            "a.torso_color_r, a.torso_color_g, a.torso_color_b, "
                            "a.left_arm_color_r, a.left_arm_color_g, a.left_arm_color_b, "
                            "a.right_arm_color_r, a.right_arm_color_g, a.right_arm_color_b, "
                            "a.left_leg_color_r, a.left_leg_color_g, a.left_leg_color_b, "
                            "a.right_leg_color_r, a.right_leg_color_g, a.right_leg_color_b "
                            "FROM users u "
                            "LEFT JOIN avatars a ON a.user_id = u.id "
                            "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
        stmt.bind(1, userId);
        if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
        profile = serializePublicUser(db_, stmt, userId);
    }
    return util::jsonResponse(200, json{{"success", true}, {"profile", profile}});
}

HttpResponse WebApp::usersSearch(const HttpRequest& request, int64_t userId) {
    const util::QueryParams params = util::parseQuery(request.query);
    std::string query = util::trim(params.get("q"));
    if (query.size() > 30) query.resize(30);

    std::vector<json> users;
    if (!query.empty()) {
        const int64_t exactId = util::parsePositiveInt(query);
        std::string likePattern = "%";
        for (char c : util::lower(query)) {
            if (c == '\\' || c == '%' || c == '_') likePattern += '\\';
            likePattern += c;
        }
        likePattern += "%";

        Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                               " FROM users u "
                               "LEFT JOIN avatars a ON a.user_id = u.id "
                               "LEFT JOIN user_stats s ON s.user_id = u.id "
                               "WHERE u.id = ? OR lower(u.username) LIKE ? ESCAPE '\\' "
                               "ORDER BY CASE WHEN lower(u.username) = lower(?) THEN 0 ELSE 1 END, "
                               "lower(u.username) LIMIT 12");
        stmt.bind(1, exactId);
        stmt.bind(2, likePattern);
        stmt.bind(3, query);
        while (stmt.ok() && stmt.step()) {
            if (stmt.getInt(0) == userId) continue;
            users.push_back(serializePublicUser(db_, stmt, userId));
        }
    } else {
        Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                               " FROM users u "
                               "LEFT JOIN avatars a ON a.user_id = u.id "
                               "LEFT JOIN user_stats s ON s.user_id = u.id "
                               "ORDER BY u.id DESC LIMIT 12");
        while (stmt.ok() && stmt.step()) {
            if (stmt.getInt(0) == userId) continue;
            users.push_back(serializePublicUser(db_, stmt, userId));
        }
    }

    return util::jsonResponse(200, json{{"success", true}, {"query", query}, {"users", users}});
}

HttpResponse WebApp::userDetail(const HttpRequest& request, int64_t viewerId, const std::string& idText) {
    (void)request;
    const int64_t targetUserId = util::parsePositiveInt(idText);
    if (targetUserId == 0) {
        return util::errorResponse(400, "User ID must be a positive number.");
    }

    json profile;
    {
        Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                               " FROM users u "
                               "LEFT JOIN avatars a ON a.user_id = u.id "
                               "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
        stmt.bind(1, targetUserId);
        if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
        profile = serializePublicUser(db_, stmt, viewerId);
    }

    json friends = json::array();
    {
        Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                               " FROM friendships f "
                               "JOIN users u ON u.id = CASE WHEN f.requester_id = ? THEN f.addressee_id "
                               "ELSE f.requester_id END "
                               "LEFT JOIN avatars a ON a.user_id = u.id "
                               "LEFT JOIN user_stats s ON s.user_id = u.id "
                               "WHERE f.status = 'accepted' AND (f.requester_id = ? OR f.addressee_id = ?) "
                               "ORDER BY lower(u.username) LIMIT 24");
        stmt.bind(1, targetUserId);
        stmt.bind(2, targetUserId);
        stmt.bind(3, targetUserId);
        while (stmt.ok() && stmt.step()) {
            friends.push_back(serializePublicUser(db_, stmt, viewerId));
        }
    }

    return util::jsonResponse(200, json{{"success", true}, {"profile", profile}, {"friends", friends}});
}

HttpResponse WebApp::friendsRequest(const HttpRequest& request, int64_t userId) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    const int64_t targetUserId = body.contains("userId") && body["userId"].is_number()
                                     ? static_cast<int64_t>(body["userId"].get<double>())
                                     : 0;
    if (targetUserId <= 0 || targetUserId == userId) {
        return util::errorResponse(400, "Choose another user to add as a friend.");
    }

    {
        Statement stmt(db_, "SELECT id FROM users WHERE id = ?");
        stmt.bind(1, targetUserId);
        if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
    }

    const FriendshipInfo friendship = fetchFriendship(db_, userId, targetUserId);
    const std::string& status = friendship.status;

    if (status == "accepted") return util::errorResponse(409, "You are already friends.");
    if (status == "pending" && friendship.requesterId == userId) {
        json profile;
        {
            Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                                    " FROM users u LEFT JOIN avatars a ON a.user_id = u.id "
                                    "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
            stmt.bind(1, targetUserId);
            if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
            profile = serializePublicUser(db_, stmt, userId);
        }
        return util::jsonResponse(200, json{{"success", true}, {"relationship", "outgoing"}, {"profile", profile}});
    }
    if (status == "pending" && friendship.requesterId == targetUserId) {
        Statement update(db_, "UPDATE friendships SET status = 'accepted', updated_at = CURRENT_TIMESTAMP WHERE id = ?");
        update.bind(1, friendship.id);
        if (!update.ok() || !update.exec()) return util::errorResponse(500, "Server error.");
        json profile;
        {
            Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                                    " FROM users u LEFT JOIN avatars a ON a.user_id = u.id "
                                    "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
            stmt.bind(1, targetUserId);
            if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
            profile = serializePublicUser(db_, stmt, userId);
        }
        return util::jsonResponse(200, json{{"success", true}, {"relationship", "friends"}, {"profile", profile}});
    }

    {
        Statement stmt(db_, "INSERT INTO friendships (requester_id, addressee_id, status) VALUES (?, ?, 'pending')");
        stmt.bind(1, userId);
        stmt.bind(2, targetUserId);
        if (!stmt.ok() || !stmt.exec()) return util::errorResponse(500, "Server error.");
    }

    json profile;
    {
        Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                               " FROM users u LEFT JOIN avatars a ON a.user_id = u.id "
                               "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
        stmt.bind(1, targetUserId);
        if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
        profile = serializePublicUser(db_, stmt, userId);
    }
    return util::jsonResponse(201, json{{"success", true}, {"relationship", "outgoing"}, {"profile", profile}});
}

HttpResponse WebApp::friendsRespond(const HttpRequest& request, int64_t userId) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    const int64_t requesterId = body.contains("userId") && body["userId"].is_number()
                                    ? static_cast<int64_t>(body["userId"].get<double>())
                                    : 0;
    const std::string action = util::lower(body.value("action", std::string()));
    if (requesterId <= 0 || (action != "accept" && action != "decline")) {
        return util::errorResponse(400, "Friend response requires a requester and action.");
    }

    Statement stmt(db_, "SELECT id, requester_id, addressee_id, status FROM friendships "
                        "WHERE requester_id = ? AND addressee_id = ? AND status = 'pending'");
    stmt.bind(1, requesterId);
    stmt.bind(2, userId);
    if (!stmt.ok() || !stmt.step()) {
        return util::errorResponse(404, "Friend request not found.");
    }
    const int64_t friendshipId = stmt.getInt(0);

    if (action == "accept") {
        Statement update(db_, "UPDATE friendships SET status = 'accepted', updated_at = CURRENT_TIMESTAMP WHERE id = ?");
        update.bind(1, friendshipId);
        if (!update.ok() || !update.exec()) return util::errorResponse(500, "Server error.");
    } else {
        Statement remove(db_, "DELETE FROM friendships WHERE id = ?");
        remove.bind(1, friendshipId);
        if (!remove.ok() || !remove.exec()) return util::errorResponse(500, "Server error.");
    }

    json profile;
    {
        Statement detail(db_, std::string("SELECT ") + kPublicUserColumns +
                                 " FROM users u LEFT JOIN avatars a ON a.user_id = u.id "
                                 "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
        detail.bind(1, requesterId);
        if (!detail.ok() || !detail.step()) return util::errorResponse(404, "User not found.");
        profile = serializePublicUser(db_, detail, userId);
    }

    return util::jsonResponse(
        200, json{{"success", true}, {"relationship", action == "accept" ? "friends" : "none"}, {"profile", profile}});
}

HttpResponse WebApp::friendsRemove(const HttpRequest& request, int64_t userId) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    const int64_t targetUserId = body.contains("userId") && body["userId"].is_number()
                                     ? static_cast<int64_t>(body["userId"].get<double>())
                                     : 0;
    if (targetUserId <= 0 || targetUserId == userId) {
        return util::errorResponse(400, "Choose another user.");
    }

    {
        Statement stmt(db_, "SELECT id FROM users WHERE id = ?");
        stmt.bind(1, targetUserId);
        if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
    }

    {
        Statement stmt(db_, "DELETE FROM friendships WHERE (requester_id = ? AND addressee_id = ?) "
                            "OR (requester_id = ? AND addressee_id = ?)");
        stmt.bind(1, userId);
        stmt.bind(2, targetUserId);
        stmt.bind(3, targetUserId);
        stmt.bind(4, userId);
        if (!stmt.ok() || !stmt.exec()) return util::errorResponse(500, "Server error.");
    }

    json profile;
    {
        Statement stmt(db_, std::string("SELECT ") + kPublicUserColumns +
                               " FROM users u LEFT JOIN avatars a ON a.user_id = u.id "
                               "LEFT JOIN user_stats s ON s.user_id = u.id WHERE u.id = ?");
        stmt.bind(1, targetUserId);
        if (!stmt.ok() || !stmt.step()) return util::errorResponse(404, "User not found.");
        profile = serializePublicUser(db_, stmt, userId);
    }
    return util::jsonResponse(200, json{{"success", true}, {"relationship", "none"}, {"profile", profile}});
}

// ---------------------------------------------------------------------------
// Avatar endpoints
// ---------------------------------------------------------------------------

HttpResponse WebApp::avatarGet(const HttpRequest& request, int64_t userId) {
    (void)request;
    Statement stmt(db_, "SELECT user_id, face_id, "
                        "head_color_r, head_color_g, head_color_b, "
                        "torso_color_r, torso_color_g, torso_color_b, "
                        "left_arm_color_r, left_arm_color_g, left_arm_color_b, "
                        "right_arm_color_r, right_arm_color_g, right_arm_color_b, "
                        "left_leg_color_r, left_leg_color_g, left_leg_color_b, "
                        "right_leg_color_r, right_leg_color_g, right_leg_color_b "
                        "FROM avatars WHERE user_id = ?");
    stmt.bind(1, userId);

    json avatar;
    if (stmt.ok() && stmt.step()) {
        auto color = [&stmt](int r, int g, int b) {
            return json::array({stmt.getDouble(r), stmt.getDouble(g), stmt.getDouble(b)});
        };
        avatar = json{
            {"headColor", color(2, 3, 4)},
            {"torsoColor", color(5, 6, 7)},
            {"leftArmColor", color(8, 9, 10)},
            {"rightArmColor", color(11, 12, 13)},
            {"leftLegColor", color(14, 15, 16)},
            {"rightLegColor", color(17, 18, 19)},
            {"faceId", stmt.isNull(1) || !util::isFaceId(stmt.getText(1)) ? "classic" : stmt.getText(1)}
        };
    } else {
        avatar = json{
            {"headColor", json::array({0.8, 0.6, 0.4})},
            {"torsoColor", json::array({0.2, 0.4, 0.8})},
            {"leftArmColor", json::array({0.8, 0.6, 0.4})},
            {"rightArmColor", json::array({0.8, 0.6, 0.4})},
            {"leftLegColor", json::array({0.2, 0.6, 0.2})},
            {"rightLegColor", json::array({0.2, 0.6, 0.2})},
            {"faceId", "classic"}
        };
    }
    return util::jsonResponse(200, json{{"success", true}, {"avatar", avatar}});
}

HttpResponse WebApp::avatarPost(const HttpRequest& request, int64_t userId) {
    json body;
    HttpResponse error;
    if (!jsonBody(request, body, error)) return error;

    AvatarPayload values{};
    std::string parseError;
    if (!parseAvatarPayload(body, values, parseError)) {
        return util::errorResponse(400, parseError);
    }

    Statement stmt(db_, "INSERT INTO avatars ("
                        "user_id, "
                        "head_color_r, head_color_g, head_color_b, "
                        "torso_color_r, torso_color_g, torso_color_b, "
                        "left_arm_color_r, left_arm_color_g, left_arm_color_b, "
                        "right_arm_color_r, right_arm_color_g, right_arm_color_b, "
                        "left_leg_color_r, left_leg_color_g, left_leg_color_b, "
                        "right_leg_color_r, right_leg_color_g, right_leg_color_b, "
                        "face_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                        "ON CONFLICT(user_id) DO UPDATE SET "
                        "head_color_r = excluded.head_color_r, head_color_g = excluded.head_color_g, "
                        "head_color_b = excluded.head_color_b, "
                        "torso_color_r = excluded.torso_color_r, torso_color_g = excluded.torso_color_g, "
                        "torso_color_b = excluded.torso_color_b, "
                        "left_arm_color_r = excluded.left_arm_color_r, "
                        "left_arm_color_g = excluded.left_arm_color_g, "
                        "left_arm_color_b = excluded.left_arm_color_b, "
                        "right_arm_color_r = excluded.right_arm_color_r, "
                        "right_arm_color_g = excluded.right_arm_color_g, "
                        "right_arm_color_b = excluded.right_arm_color_b, "
                        "left_leg_color_r = excluded.left_leg_color_r, "
                        "left_leg_color_g = excluded.left_leg_color_g, "
                        "left_leg_color_b = excluded.left_leg_color_b, "
                        "right_leg_color_r = excluded.right_leg_color_r, "
                        "right_leg_color_g = excluded.right_leg_color_g, "
                        "right_leg_color_b = excluded.right_leg_color_b, "
                        "face_id = excluded.face_id");

    stmt.bind(1, userId);
    stmt.bind(2, values.head[0]);
    stmt.bind(3, values.head[1]);
    stmt.bind(4, values.head[2]);
    stmt.bind(5, values.torso[0]);
    stmt.bind(6, values.torso[1]);
    stmt.bind(7, values.torso[2]);
    stmt.bind(8, values.leftArm[0]);
    stmt.bind(9, values.leftArm[1]);
    stmt.bind(10, values.leftArm[2]);
    stmt.bind(11, values.rightArm[0]);
    stmt.bind(12, values.rightArm[1]);
    stmt.bind(13, values.rightArm[2]);
    stmt.bind(14, values.leftLeg[0]);
    stmt.bind(15, values.leftLeg[1]);
    stmt.bind(16, values.leftLeg[2]);
    stmt.bind(17, values.rightLeg[0]);
    stmt.bind(18, values.rightLeg[1]);
    stmt.bind(19, values.rightLeg[2]);
    stmt.bind(20, values.faceId);

    if (!stmt.ok() || !stmt.exec()) return util::errorResponse(500, "Server error.");
    return util::jsonResponse(200, json{{"success", true}});
}

} // namespace web
