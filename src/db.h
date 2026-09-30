// Thin SQLite access layer over sql/schema.sql. All statements are
// prepared/bound (no string-concatenated SQL) to rule out injection from
// user- or Google-supplied strings (email, display name, etc).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace lugbulk {

struct User {
    int64_t id;
    std::string google_sub;
    std::string email;
};

struct Session {
    std::string token;  // raw token for the cookie; the DB stores only its hash
    int64_t user_id;
    std::string expires_at;  // ISO8601 UTC, "YYYY-MM-DD HH:MM:SS"
};

struct Sheet {
    int64_t id;
    int64_t user_id;
    std::string sheet_id;      // Google Sheets/Drive file id
    std::string display_name;
};

// A saved label design (see sql/schema.sql `sheet_designs`).
struct Design {
    std::string label_spec;    // LabelSpec id
    std::string part_order;    // "heaviest" | "lightest" | "sheet"
    std::string hidden_parts;  // comma-separated LabelPart names
};

// One row of the run history log.
struct Run {
    std::string report_type;  // "labels" | "lot_counts" | "parts"
    std::string generated_at;  // "YYYY-MM-DD HH:MM:SS", UTC
    int item_count;
    std::string status;  // "ok" | "error"
    std::string error_message;
};

// A cached BrickLink catalog lookup (see src/bricklink.h).
struct BrickLinkPart {
    std::string element_id;
    std::string part_no;  // empty if BrickLink doesn't know the element
    std::string color;
    std::optional<double> weight;
    int64_t fetched_at;  // unix time
};

// Mirrors the `sheets` row's owner check needed before generating against it.
struct SheetOwnership {
    int64_t sheet_row_id;
    std::string sheet_id;      // Google Sheets file id
    std::string display_name;
    bool verified = false;     // user's Google access to it has been confirmed
};

class Db {
public:
    // Opens (creating if needed) the sqlite file at `path` and applies
    // schema.sql (all statements are CREATE TABLE/INDEX IF NOT EXISTS, so
    // this is safe to run on every startup). Throws std::runtime_error on
    // failure — message includes the sqlite error text but never row data.
    explicit Db(const std::string& path, const std::string& schema_sql_path);
    ~Db();

    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // Looks up a user by Google `sub`, or creates one if absent. Always
    // overwrites the stored encrypted refresh token and last_login_at —
    // Google only issues a refresh token on first consent / when
    // `prompt=consent` is forced, so callers should only pass a non-empty
    // `refresh_token_enc` when one was actually returned this round trip.
    User upsert_user(const std::string& google_sub, const std::string& email,
                      const std::vector<uint8_t>* refresh_token_enc);

    std::optional<User> find_user_by_id(int64_t id);

    // Raw encrypted refresh-token blob for a user, straight from storage —
    // callers must decrypt via crypto::aes_gcm_decrypt before use, and must
    // not log or persist the decrypted value beyond the single outbound
    // Google API call it's used for.
    std::optional<std::vector<uint8_t>> get_refresh_token_enc(int64_t user_id);

    // Session helpers. `ttl_seconds` controls expires_at.
    Session create_session(int64_t user_id, int ttl_seconds);
    std::optional<User> find_user_by_session(const std::string& token);
    void delete_session(const std::string& token);
    void delete_expired_sessions();
    // Deletes all but the newest `keep` sessions of a user.
    void trim_sessions(int64_t user_id, int keep);

    // Saves (or, if already saved, renames) a sheet the user picked from
    // the Drive search/list result under their own account.
    Sheet add_sheet(int64_t user_id, const std::string& sheet_id,
                     const std::string& display_name);
    std::vector<Sheet> list_sheets(int64_t user_id);
    size_t count_sheets(int64_t user_id);
    // Only deletes if the sheet belongs to `user_id` — callers must not
    // trust a bare sheet row id from a request without this ownership check.
    bool delete_sheet(int64_t user_id, int64_t sheet_row_id);

    // Looks up a saved sheet by row id, scoped to `user_id` — the
    // ownership check every /sheets/:id/{labels,lots} route needs before
    // touching a sheet_id a request only supplied as a bare row id.
    std::optional<SheetOwnership> find_owned_sheet(int64_t user_id, int64_t sheet_row_id);

    // Appends a row to the run history log (see sql/schema.sql `runs`).
    // Never stores the generated file itself — see README's data model note.
    void log_run(int64_t sheet_row_id, const std::string& report_type, int item_count,
                 const std::string& status, const std::string* error_message);

    // Most recent runs for a sheet, newest first. Callers must have done
    // the find_owned_sheet ownership check.
    std::vector<Run> list_runs(int64_t sheet_row_id, int limit);

    // Records that the user's Google access to a saved sheet was confirmed.
    void mark_sheet_verified(int64_t sheet_row_id);

    // Label design for a Google Sheet, shared across users; nullopt if none saved.
    std::optional<Design> get_design(const std::string& google_sheet_id);
    void put_design(const std::string& google_sheet_id, const Design& design, int64_t user_id);

    // BrickLink lookup cache.
    std::map<std::string, BrickLinkPart> get_bricklink_parts(const std::vector<std::string>& element_ids);
    void put_bricklink_part(const BrickLinkPart& part);

private:
    void migrate();

    sqlite3* db_ = nullptr;
};

}  // namespace lugbulk
