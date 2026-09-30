#include "db.h"

#include <sqlite3.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "crypto.h"

namespace lugbulk {

namespace {

[[noreturn]] void throw_sqlite_error(sqlite3* db, const char* what) {
    std::string msg = std::string("db error: ") + what;
    if (db) {
        msg += ": ";
        msg += sqlite3_errmsg(db);
    }
    throw std::runtime_error(msg);
}

// RAII wrapper so every early-return path still finalizes the statement.
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            throw_sqlite_error(db, "prepare failed");
        }
    }
    ~Stmt() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind_text(int idx, const std::string& v) {
        if (sqlite3_bind_text(stmt_, idx, v.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            throw_sqlite_error(db_, "bind_text failed");
    }
    void bind_blob(int idx, const std::vector<uint8_t>& v) {
        int rc = v.empty()
                     ? sqlite3_bind_zeroblob(stmt_, idx, 0)
                     : sqlite3_bind_blob(stmt_, idx, v.data(), static_cast<int>(v.size()),
                                         SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) throw_sqlite_error(db_, "bind_blob failed");
    }
    void bind_null(int idx) {
        if (sqlite3_bind_null(stmt_, idx) != SQLITE_OK) throw_sqlite_error(db_, "bind_null failed");
    }
    void bind_int64(int idx, int64_t v) {
        if (sqlite3_bind_int64(stmt_, idx, v) != SQLITE_OK)
            throw_sqlite_error(db_, "bind_int64 failed");
    }
    void bind_int(int idx, int v) {
        if (sqlite3_bind_int(stmt_, idx, v) != SQLITE_OK) throw_sqlite_error(db_, "bind_int failed");
    }

    void bind_double(int idx, double v) {
        if (sqlite3_bind_double(stmt_, idx, v) != SQLITE_OK)
            throw_sqlite_error(db_, "bind_double failed");
    }
    // Rewinds for re-execution with new bindings.
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    bool column_is_null(int idx) { return sqlite3_column_type(stmt_, idx) == SQLITE_NULL; }
    double column_double(int idx) { return sqlite3_column_double(stmt_, idx); }

    // Returns true if a row is available (SQLITE_ROW), false on SQLITE_DONE.
    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw_sqlite_error(db_, "step failed");
    }

    std::string column_text(int idx) {
        const unsigned char* t = sqlite3_column_text(stmt_, idx);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    }
    int64_t column_int64(int idx) { return sqlite3_column_int64(stmt_, idx); }
    std::vector<uint8_t> column_blob(int idx) {
        const void* data = sqlite3_column_blob(stmt_, idx);
        int len = sqlite3_column_bytes(stmt_, idx);
        if (!data || len <= 0) return {};
        const auto* bytes = static_cast<const uint8_t*>(data);
        return std::vector<uint8_t>(bytes, bytes + len);
    }

    sqlite3_stmt* raw() { return stmt_; }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("db error: could not open schema file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

Db::Db(const std::string& path, const std::string& schema_sql_path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = "db error: could not open database";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(msg);
    }
    // Enforce FK constraints on this connection (schema.sql also sets this,
    // but that PRAGMA is per-connection, not persisted in the file).
    char* errmsg = nullptr;
    if (sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string msg = std::string("db error: enabling foreign_keys failed: ") +
                           (errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        throw std::runtime_error(msg);
    }

    std::string schema = read_file(schema_sql_path);
    if (sqlite3_exec(db_, schema.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
        std::string msg = std::string("db error: schema apply failed: ") +
                           (errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        throw std::runtime_error(msg);
    }
    migrate();
}

// Brings databases created by older builds up to schema.sql, which only
// ever CREATEs IF NOT EXISTS and so can't change an existing table.
void Db::migrate() {
    // The BrickLink API lookup cache, from before the catalog-file import.
    sqlite3_exec(db_, "DROP TABLE IF EXISTS bricklink_parts;", nullptr, nullptr, nullptr);
    // sheets.verified_at (added with shared label designs).
    {
        bool has_verified = false;
        Stmt s(db_, "SELECT name FROM pragma_table_info('sheets');");
        while (s.step()) has_verified |= s.column_text(0) == "verified_at";
        if (!has_verified) {
            char* errmsg = nullptr;
            if (sqlite3_exec(db_, "ALTER TABLE sheets ADD COLUMN verified_at TEXT;", nullptr,
                             nullptr, &errmsg) != SQLITE_OK) {
                std::string msg = std::string("db error: sheets migration failed: ") +
                                  (errmsg ? errmsg : "unknown");
                sqlite3_free(errmsg);
                throw std::runtime_error(msg);
            }
        }
    }
    // sheet_designs.report_options (added with the browser-side reports).
    {
        bool has_options = false;
        Stmt s(db_, "SELECT name FROM pragma_table_info('sheet_designs');");
        while (s.step()) has_options |= s.column_text(0) == "report_options";
        if (!has_options) {
            char* errmsg = nullptr;
            if (sqlite3_exec(db_, "ALTER TABLE sheet_designs ADD COLUMN report_options TEXT;",
                             nullptr, nullptr, &errmsg) != SQLITE_OK) {
                std::string msg = std::string("db error: sheet_designs migration failed: ") +
                                  (errmsg ? errmsg : "unknown");
                sqlite3_free(errmsg);
                throw std::runtime_error(msg);
            }
        }
    }
    // sheet_designs.keep_parts (added with "Keep each part on one sheet").
    {
        bool has_keep = false;
        Stmt s(db_, "SELECT name FROM pragma_table_info('sheet_designs');");
        while (s.step()) has_keep |= s.column_text(0) == "keep_parts";
        if (!has_keep) {
            char* errmsg = nullptr;
            if (sqlite3_exec(db_,
                             "ALTER TABLE sheet_designs ADD COLUMN keep_parts TEXT NOT NULL "
                             "DEFAULT 'off' CHECK (keep_parts IN ('off', 'optimize'));",
                             nullptr, nullptr, &errmsg) != SQLITE_OK) {
                std::string msg = std::string("db error: sheet_designs migration failed: ") +
                                  (errmsg ? errmsg : "unknown");
                sqlite3_free(errmsg);
                throw std::runtime_error(msg);
            }
        }
    }
    std::string runs_sql;
    {
        Stmt s(db_, "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'runs';");
        if (s.step()) runs_sql = s.column_text(0);
    }
    // Older runs.report_type CHECKs lacked 'parts' / 'checklist' / 'bundle'. SQLite
    // can't alter a CHECK constraint, so rebuild the table.
    if (!runs_sql.empty() && runs_sql.find("'bundle'") == std::string::npos) {
        const char* sql =
            "BEGIN;"
            "CREATE TABLE runs_new ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  sheet_id INTEGER NOT NULL REFERENCES sheets(id) ON DELETE CASCADE,"
            "  report_type TEXT NOT NULL CHECK (report_type IN ('labels', 'lot_counts', 'parts', "
            "'checklist', 'bundle')),"
            "  generated_at TEXT NOT NULL DEFAULT (datetime('now')),"
            "  item_count INTEGER NOT NULL,"
            "  status TEXT NOT NULL CHECK (status IN ('ok', 'error')),"
            "  error_message TEXT);"
            "INSERT INTO runs_new SELECT id, sheet_id, report_type, generated_at, item_count, "
            "  status, error_message FROM runs;"
            "DROP TABLE runs;"
            "ALTER TABLE runs_new RENAME TO runs;"
            "CREATE INDEX IF NOT EXISTS idx_runs_sheet ON runs(sheet_id, generated_at DESC);"
            "COMMIT;";
        char* errmsg = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg) != SQLITE_OK) {
            std::string msg = std::string("db error: runs migration failed: ") +
                              (errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            throw std::runtime_error(msg);
        }
    }
}

Db::~Db() {
    if (db_) sqlite3_close(db_);
}

User Db::upsert_user(const std::string& google_sub, const std::string& email,
                      const std::vector<uint8_t>* refresh_token_enc) {
    if (refresh_token_enc) {
        // New or re-consented login: we have a fresh refresh token to store.
        Stmt s(db_,
               "INSERT INTO users (google_sub, email, refresh_token_enc, last_login_at) "
               "VALUES (?, ?, ?, datetime('now')) "
               "ON CONFLICT(google_sub) DO UPDATE SET "
               "  email = excluded.email, "
               "  refresh_token_enc = excluded.refresh_token_enc, "
               "  last_login_at = datetime('now') "
               "RETURNING id, google_sub, email;");
        s.bind_text(1, google_sub);
        s.bind_text(2, email);
        s.bind_blob(3, *refresh_token_enc);
        if (!s.step()) throw_sqlite_error(db_, "upsert_user RETURNING produced no row");
        User u;
        u.id = s.column_int64(0);
        u.google_sub = s.column_text(1);
        u.email = s.column_text(2);
        return u;
    }

    // Returning login, no new refresh token from Google this time — update
    // email/last_login_at only, leave the stored refresh token untouched.
    // Requires the user to already exist (first login always supplies a
    // refresh token via the `prompt=consent` flow).
    {
        Stmt s(db_,
               "UPDATE users SET email = ?, last_login_at = datetime('now') "
               "WHERE google_sub = ? "
               "RETURNING id, google_sub, email;");
        s.bind_text(1, email);
        s.bind_text(2, google_sub);
        if (s.step()) {
            User u;
            u.id = s.column_int64(0);
            u.google_sub = s.column_text(1);
            u.email = s.column_text(2);
            return u;
        }
    }
    throw std::runtime_error(
        "db error: login for unknown user without a refresh token (consent required)");
}

std::optional<User> Db::find_user_by_id(int64_t id) {
    Stmt s(db_, "SELECT id, google_sub, email FROM users WHERE id = ?;");
    s.bind_int64(1, id);
    if (!s.step()) return std::nullopt;
    User u;
    u.id = s.column_int64(0);
    u.google_sub = s.column_text(1);
    u.email = s.column_text(2);
    return u;
}

std::optional<std::vector<uint8_t>> Db::get_refresh_token_enc(int64_t user_id) {
    Stmt s(db_, "SELECT refresh_token_enc FROM users WHERE id = ?;");
    s.bind_int64(1, user_id);
    if (!s.step()) return std::nullopt;
    return s.column_blob(0);
}

Session Db::create_session(int64_t user_id, int ttl_seconds) {
    std::string token = crypto::random_hex_token(32);  // 256 bits, unguessable
    // Only the hash is stored; the raw token lives in the user's cookie.
    Stmt s(db_,
           "INSERT INTO sessions (token, user_id, expires_at) "
           "VALUES (?, ?, datetime('now', ?)) "
           "RETURNING user_id, expires_at;");
    s.bind_text(1, crypto::sha256_hex(token));
    s.bind_int64(2, user_id);
    s.bind_text(3, "+" + std::to_string(ttl_seconds) + " seconds");
    if (!s.step()) throw_sqlite_error(db_, "create_session RETURNING produced no row");
    Session sess;
    sess.token = token;
    sess.user_id = s.column_int64(0);
    sess.expires_at = s.column_text(1);
    return sess;
}

std::optional<User> Db::find_user_by_session(const std::string& token) {
    Stmt s(db_,
           "SELECT u.id, u.google_sub, u.email "
           "FROM sessions s JOIN users u ON u.id = s.user_id "
           "WHERE s.token = ? AND s.expires_at > datetime('now');");
    s.bind_text(1, crypto::sha256_hex(token));
    if (!s.step()) return std::nullopt;
    User u;
    u.id = s.column_int64(0);
    u.google_sub = s.column_text(1);
    u.email = s.column_text(2);
    return u;
}

void Db::delete_session(const std::string& token) {
    Stmt s(db_, "DELETE FROM sessions WHERE token = ?;");
    s.bind_text(1, crypto::sha256_hex(token));
    s.step();
}

void Db::delete_expired_sessions() {
    Stmt s(db_, "DELETE FROM sessions WHERE expires_at <= datetime('now');");
    s.step();
}

void Db::trim_sessions(int64_t user_id, int keep) {
    Stmt s(db_,
           "DELETE FROM sessions WHERE user_id = ? AND token NOT IN ("
           "  SELECT token FROM sessions WHERE user_id = ? "
           "  ORDER BY created_at DESC, rowid DESC LIMIT ?);");
    s.bind_int64(1, user_id);
    s.bind_int64(2, user_id);
    s.bind_int(3, keep);
    s.step();
}

size_t Db::count_sheets(int64_t user_id) {
    Stmt s(db_, "SELECT COUNT(*) FROM sheets WHERE user_id = ?;");
    s.bind_int64(1, user_id);
    s.step();
    return static_cast<size_t>(s.column_int64(0));
}

Sheet Db::add_sheet(int64_t user_id, const std::string& sheet_id,
                     const std::string& display_name) {
    // schema.sql has UNIQUE(user_id, sheet_id); re-adding an already-saved
    // sheet just updates its display name rather than erroring, since the
    // caller is re-selecting it from a picker, not intentionally duplicating.
    Stmt s(db_,
           "INSERT INTO sheets (user_id, sheet_id, display_name, verified_at) "
           "VALUES (?, ?, ?, datetime('now')) "
           "ON CONFLICT(user_id, sheet_id) DO UPDATE SET display_name = excluded.display_name, "
           "  verified_at = excluded.verified_at "
           "RETURNING id, user_id, sheet_id, display_name;");
    s.bind_int64(1, user_id);
    s.bind_text(2, sheet_id);
    s.bind_text(3, display_name);
    if (!s.step()) throw_sqlite_error(db_, "add_sheet RETURNING produced no row");
    Sheet sheet;
    sheet.id = s.column_int64(0);
    sheet.user_id = s.column_int64(1);
    sheet.sheet_id = s.column_text(2);
    sheet.display_name = s.column_text(3);
    return sheet;
}

std::vector<Sheet> Db::list_sheets(int64_t user_id) {
    Stmt s(db_,
           "SELECT id, user_id, sheet_id, display_name FROM sheets "
           "WHERE user_id = ? ORDER BY added_at DESC;");
    s.bind_int64(1, user_id);
    std::vector<Sheet> out;
    while (s.step()) {
        Sheet sheet;
        sheet.id = s.column_int64(0);
        sheet.user_id = s.column_int64(1);
        sheet.sheet_id = s.column_text(2);
        sheet.display_name = s.column_text(3);
        out.push_back(std::move(sheet));
    }
    return out;
}

bool Db::delete_sheet(int64_t user_id, int64_t sheet_row_id) {
    // RETURNING rather than sqlite3_changes(): the connection is shared by
    // Crow's worker threads, so changes() could report another request's write.
    Stmt s(db_, "DELETE FROM sheets WHERE id = ? AND user_id = ? RETURNING id;");
    s.bind_int64(1, sheet_row_id);
    s.bind_int64(2, user_id);
    return s.step();
}

std::optional<SheetOwnership> Db::find_owned_sheet(int64_t user_id, int64_t sheet_row_id) {
    Stmt s(db_,
           "SELECT id, sheet_id, display_name, verified_at IS NOT NULL FROM sheets "
           "WHERE id = ? AND user_id = ?;");
    s.bind_int64(1, sheet_row_id);
    s.bind_int64(2, user_id);
    if (!s.step()) return std::nullopt;
    SheetOwnership so;
    so.sheet_row_id = s.column_int64(0);
    so.sheet_id = s.column_text(1);
    so.display_name = s.column_text(2);
    so.verified = s.column_int64(3) != 0;
    return so;
}

void Db::mark_sheet_verified(int64_t sheet_row_id) {
    Stmt s(db_, "UPDATE sheets SET verified_at = datetime('now') WHERE id = ?;");
    s.bind_int64(1, sheet_row_id);
    s.step();
}

std::optional<Design> Db::get_design(const std::string& google_sheet_id) {
    Stmt s(db_,
           "SELECT label_spec, part_order, hidden_parts, COALESCE(report_options, ''), keep_parts "
           "FROM sheet_designs WHERE google_sheet_id = ?;");
    s.bind_text(1, google_sheet_id);
    if (!s.step()) return std::nullopt;
    return Design{s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                  s.column_text(4)};
}

void Db::put_design(const std::string& google_sheet_id, const Design& design, int64_t user_id) {
    Stmt s(db_,
           "INSERT INTO sheet_designs (google_sheet_id, label_spec, part_order, hidden_parts, "
           "  updated_by, updated_at, report_options) "
           "VALUES (?, ?, ?, ?, ?, datetime('now'), ?) "
           "ON CONFLICT(google_sheet_id) DO UPDATE SET label_spec = excluded.label_spec, "
           "  part_order = excluded.part_order, hidden_parts = excluded.hidden_parts, "
           "  updated_by = excluded.updated_by, updated_at = excluded.updated_at, "
           "  report_options = COALESCE(excluded.report_options, sheet_designs.report_options);");
    s.bind_text(1, google_sheet_id);
    s.bind_text(2, design.label_spec);
    s.bind_text(3, design.part_order);
    s.bind_text(4, design.hidden_parts);
    s.bind_int64(5, user_id);
    if (design.report_options) {
        s.bind_text(6, *design.report_options);
    } else {
        s.bind_null(6);
    }
    s.step();
    if (design.keep_parts) {
        Stmt keep(db_, "UPDATE sheet_designs SET keep_parts = ? WHERE google_sheet_id = ?;");
        keep.bind_text(1, *design.keep_parts);
        keep.bind_text(2, google_sheet_id);
        keep.step();
    }
}

std::vector<Run> Db::list_runs(int64_t sheet_row_id, int limit) {
    Stmt s(db_,
           "SELECT report_type, generated_at, item_count, status, COALESCE(error_message, '') "
           "FROM runs WHERE sheet_id = ? ORDER BY generated_at DESC, id DESC LIMIT ?;");
    s.bind_int64(1, sheet_row_id);
    s.bind_int(2, limit);
    std::vector<Run> out;
    while (s.step()) {
        out.push_back({s.column_text(0), s.column_text(1), static_cast<int>(s.column_int64(2)),
                       s.column_text(3), s.column_text(4)});
    }
    return out;
}

void Db::log_run(int64_t sheet_row_id, const std::string& report_type, int item_count,
                  const std::string& status, const std::string* error_message) {
    Stmt s(db_,
           "INSERT INTO runs (sheet_id, report_type, item_count, status, error_message) "
           "VALUES (?, ?, ?, ?, ?);");
    s.bind_int64(1, sheet_row_id);
    s.bind_text(2, report_type);
    s.bind_int(3, item_count);
    s.bind_text(4, status);
    if (error_message) {
        s.bind_text(5, *error_message);
    } else {
        s.bind_null(5);
    }
    s.step();
}

}  // namespace lugbulk
