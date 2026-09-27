#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace web {

// Thin RAII wrapper over sqlite3 prepared statements used by the web app.
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
    }
    explicit Statement(sqlite3* db, const std::string& sql) : Statement(db, sql.c_str()) {}
    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool ok() const { return stmt_ != nullptr; }

    void bind(int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    void bind(int index, int64_t value) { sqlite3_bind_int64(stmt_, index, value); }
    void bind(int index, int value) { sqlite3_bind_int64(stmt_, index, value); }
    void bind(int index, double value) { sqlite3_bind_double(stmt_, index, value); }

    // Returns true while a row is available.
    bool step() { return sqlite3_step(stmt_) == SQLITE_ROW; }

    // Runs an INSERT/UPDATE/DELETE to completion.
    bool exec() {
        const int rc = sqlite3_step(stmt_);
        return rc == SQLITE_DONE || rc == SQLITE_ROW;
    }

    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

    int64_t getInt(int column) const { return sqlite3_column_int64(stmt_, column); }
    double getDouble(int column) const { return sqlite3_column_double(stmt_, column); }
    std::string getText(int column) const {
        const unsigned char* value = sqlite3_column_text(stmt_, column);
        return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
    }
    bool isNull(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

inline int64_t lastInsertRowId(sqlite3* db) {
    return sqlite3_last_insert_rowid(db);
}

inline int lastChanges(sqlite3* db) {
    return sqlite3_changes(db);
}

} // namespace web
