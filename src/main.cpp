// lugbulk-labels-web — hosted counterpart to the lugbulk-label CLI.
//
// Routes so far:
//   GET    /                    dashboard HTML, mustache-rendered (redirects to /auth/login if not logged in)
//   GET    /healthz             liveness check
//   GET    /auth/login          kick off Google OAuth
//   GET    /auth/callback       OAuth redirect target, stores refresh token
//   POST   /auth/logout         clears the session
//   GET    /sheets/search       search the user's Drive for spreadsheets (?q=)
//   GET    /sheets              list sheets the user has saved
//   POST   /sheets              save a sheet the user picked (id + display name)
//   DELETE /sheets/:row_id      remove a saved sheet
//   POST   /sheets/:id/labels   generate labels.pdf for a sheet, synchronous
//   POST   /sheets/:id/lots     generate lot_counts.csv (default) or .pdf (?format=pdf)
// Still to come:
//   GET  /sheets/:id/history  run log for a sheet
//
// See sql/schema.sql for the users/sheets/runs/sessions tables.

#include "crow.h"
#include "crow/json.h"
#include "crow/mustache.h"

#include <cctype>
#include <iostream>
#include <optional>
#include <vector>

#include "config.h"
#include "crypto.h"
#include "db.h"
#include "labels_pdf.h"
#include "oauth.h"
#include "reports.h"
#include "sheet_layout.h"
#include "sheet_pivot.h"

namespace {

using namespace lugbulk;

constexpr int kSessionTtlSeconds = 30 * 24 * 60 * 60;  // 30 days
constexpr int kStateTtlSeconds = 10 * 60;              // OAuth round trip window
constexpr const char* kSessionCookie = "lugbulk_session";
constexpr const char* kStateCookie = "lugbulk_oauth_state";

// Cookie flags shared by every cookie we set. `Secure` is conditional on
// the redirect URI being https (so local http://localhost dev still works;
// anything reachable over the network should be behind TLS in production —
// see README's OAuth setup, production redirect URIs are expected to be
// https).
std::string cookie_attrs(const Config& cfg, int max_age_seconds) {
    std::string attrs = "Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
                         std::to_string(max_age_seconds);
    if (cfg.google_redirect_uri.rfind("https://", 0) == 0) {
        attrs += "; Secure";
    }
    return attrs;
}

std::string clear_cookie_attrs(const Config& cfg) {
    std::string attrs = "Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
    if (cfg.google_redirect_uri.rfind("https://", 0) == 0) {
        attrs += "; Secure";
    }
    return attrs;
}

std::optional<std::string> get_cookie(const crow::request& req, const std::string& name) {
    std::string cookie_header = req.get_header_value("Cookie");
    if (cookie_header.empty()) return std::nullopt;

    size_t pos = 0;
    while (pos < cookie_header.size()) {
        size_t sep = cookie_header.find(';', pos);
        std::string part = cookie_header.substr(pos, sep == std::string::npos ? std::string::npos
                                                                               : sep - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos) {
            std::string key = part.substr(0, eq);
            // trim leading spaces
            size_t start = key.find_first_not_of(' ');
            if (start != std::string::npos) key = key.substr(start);
            if (key == name) {
                return part.substr(eq + 1);
            }
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return std::nullopt;
}

std::optional<User> current_user(Db& db, const crow::request& req) {
    auto token = get_cookie(req, kSessionCookie);
    if (!token) return std::nullopt;
    return db.find_user_by_session(*token);
}

// Decrypts the user's stored refresh token and exchanges it for a fresh,
// short-lived access token. The access token is returned to the caller for
// one immediate outbound Google API call and is never written to the DB or
// logs; the decrypted refresh token similarly never leaves this function's
// stack. Throws std::runtime_error if the user has no stored refresh token
// or the refresh grant fails (e.g. user revoked access on Google's side).
std::string mint_access_token(const Config& cfg, Db& db, int64_t user_id) {
    auto enc = db.get_refresh_token_enc(user_id);
    if (!enc || enc->empty()) {
        throw std::runtime_error("no stored Google refresh token for this user");
    }
    std::string refresh_token = crypto::aes_gcm_decrypt(cfg.token_encryption_key_b64, *enc);
    oauth::TokenResponse tr = oauth::refresh_access_token(cfg, refresh_token);
    return tr.access_token;
}

// JSON string escaping for values we interpolate into hand-built JSON
// responses below (sheet/file names are arbitrary user- or Google-supplied
// text and must not be able to break out of a JSON string).
std::string json_escape(const std::string& s) {
    std::string out;
    crow::json::escape(s, out);
    return out;
}

// Turns a sheet's display name (arbitrary user-chosen text, originally a
// Drive file name) into a safe download filename: ASCII alnum/-/_/space
// only, everything else collapsed to '_'. This isn't just cosmetic — the
// name is embedded in a Content-Disposition header, so it must not be able
// to contain CR/LF (header injection) or double quotes (would break out of
// the filename="..." value); stripping to a narrow allowlist rules out
// both without needing to track escaping rules for the header grammar.
std::string safe_filename_stem(const std::string& display_name) {
    std::string out;
    out.reserve(display_name.size());
    for (unsigned char c : display_name) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == ' ') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    // Trim leading/trailing space/underscore left over from replaced runs.
    size_t start = out.find_first_not_of(" _");
    size_t end = out.find_last_not_of(" _");
    if (start == std::string::npos) return "sheet";
    return out.substr(start, end - start + 1);
}

// Fetches the "Order Here" tab for a sheet the user owns and pivots it.
// Shared by both /labels and /lots generate routes. Throws
// std::runtime_error (from mint_access_token / oauth calls) on any Google
// API failure — callers turn that into a run-log "error" row + a 502.
PivotResult fetch_and_pivot(const Config& cfg, Db& db, int64_t user_id,
                             const std::string& spreadsheet_id) {
    std::string access_token = mint_access_token(cfg, db, user_id);
    // Column count has grown across sheet years (2023: 93 cols -> 2026: 98
    // cols, as the roster grows) — ZZ (702 columns) gives a wide margin
    // against a fixed cutoff silently truncating future, larger rosters.
    std::string range = "'" + std::string(layout::kSourceTab) + "'!A1:ZZ";
    std::vector<std::vector<std::string>> rows =
        oauth::fetch_sheet_values(access_token, spreadsheet_id, range);
    return pivot_sheet(rows);
}

}  // namespace

int main() {
    Config cfg;
    try {
        cfg = Config::load_from_env();
    } catch (const std::exception& e) {
        std::cerr << "startup failed: " << e.what() << std::endl;
        return 1;
    }

    std::string db_path = cfg.data_dir + "/lugbulk.sqlite3";
    std::unique_ptr<Db> db;
    try {
        db = std::make_unique<Db>(db_path, "sql/schema.sql");
    } catch (const std::exception& e) {
        std::cerr << "startup failed: " << e.what() << std::endl;
        return 1;
    }

    crow::SimpleApp app;
    // Crow's default INFO access log prints the full request path, which
    // for /auth/callback includes the (single-use, but still sensitive)
    // authorization code and session-bound state as a query string. Drop
    // to Warning so that never lands in logs/log aggregators.
    app.loglevel(crow::LogLevel::Warning);
    crow::mustache::set_global_base("templates");

    CROW_ROUTE(app, "/healthz")([]() {
        return crow::response(200, "ok");
    });

    CROW_ROUTE(app, "/")([&](const crow::request& req) {
        auto user = current_user(*db, req);
        if (!user) {
            crow::response res(302);
            res.set_header("Location", "/auth/login");
            return res;
        }
        // Mustache HTML-escapes {{email}} automatically, so a display name
        // containing markup can't break out of the page.
        auto tmpl = crow::mustache::load("dashboard.html");
        crow::mustache::context ctx;
        ctx["email"] = user->email;
        crow::response res(200, tmpl.render(ctx));
        res.set_header("Content-Type", "text/html; charset=utf-8");
        return res;
    });

    // Step 1: redirect the browser to Google's consent screen. A random
    // `state` value is generated, stashed in a short-lived HttpOnly cookie,
    // and echoed back by Google in the callback — compared there to guard
    // against CSRF (an attacker linking a victim straight into /auth/callback
    // with an authorization code of the attacker's own account).
    CROW_ROUTE(app, "/auth/login")([&cfg](const crow::request&) {
        std::string state = crypto::random_hex_token(24);
        std::string url = oauth::build_authorize_url(cfg, state, /*force_consent=*/true);

        crow::response res(302);
        res.set_header("Location", url);
        res.add_header("Set-Cookie", std::string(kStateCookie) + "=" + state + "; " +
                                          cookie_attrs(cfg, kStateTtlSeconds));
        return res;
    });

    // Step 2: Google redirects back here with ?code=...&state=....
    CROW_ROUTE(app, "/auth/callback")([&cfg, &db](const crow::request& req) {
        auto error_param = req.url_params.get("error");
        if (error_param) {
            // User declined consent, or Google reported a problem — no
            // secrets in this path, safe to echo the fixed error code.
            return crow::response(400, std::string("OAuth error: ") + error_param);
        }

        auto code = req.url_params.get("code");
        auto returned_state = req.url_params.get("state");
        if (!code || !returned_state) {
            return crow::response(400, "missing code or state");
        }

        auto expected_state = get_cookie(req, kStateCookie);
        if (!expected_state || *expected_state != std::string(returned_state)) {
            return crow::response(400, "invalid or expired OAuth state");
        }

        try {
            oauth::TokenResponse tokens = oauth::exchange_code(cfg, code);
            oauth::UserInfo info = oauth::fetch_userinfo(tokens.access_token);

            User user{};
            if (!tokens.refresh_token.empty()) {
                std::vector<uint8_t> enc =
                    crypto::aes_gcm_encrypt(cfg.token_encryption_key_b64, tokens.refresh_token);
                user = db->upsert_user(info.sub, info.email, &enc);
            } else {
                // Returning user, Google didn't re-issue a refresh token
                // this time (expected on repeat consent without
                // prompt=consent forced — we always force it above, so
                // this path mainly guards against Google's behavior
                // changing / a user with a pre-existing row).
                user = db->upsert_user(info.sub, info.email, nullptr);
            }

            Session session = db->create_session(user.id, kSessionTtlSeconds);

            crow::response res(302);
            res.set_header("Location", "/");
            res.add_header("Set-Cookie", std::string(kSessionCookie) + "=" + session.token +
                                              "; " + cookie_attrs(cfg, kSessionTtlSeconds));
            // Clear the one-time state cookie now that the round trip is done.
            res.add_header("Set-Cookie",
                            std::string(kStateCookie) + "=; " + clear_cookie_attrs(cfg));
            return res;
        } catch (const std::exception& e) {
            // Log server-side for diagnosis; never put exception text
            // (could echo transport details) directly in the response.
            std::cerr << "auth/callback failed: " << e.what() << std::endl;
            return crow::response(502, "login failed, please try again");
        }
    });

    CROW_ROUTE(app, "/auth/logout").methods(crow::HTTPMethod::Post)(
        [&cfg, &db](const crow::request& req) {
            auto token = get_cookie(req, kSessionCookie);
            if (token) {
                db->delete_session(*token);
            }
            crow::response res(302);
            res.set_header("Location", "/");
            res.add_header("Set-Cookie",
                            std::string(kSessionCookie) + "=; " + clear_cookie_attrs(cfg));
            return res;
        });

    // Search/list the user's own Drive for spreadsheets they can pick from
    // (does not touch our `sheets` table — this is live Drive metadata,
    // not what's already saved). `?q=` is an optional name substring.
    CROW_ROUTE(app, "/sheets/search")([&cfg, &db](const crow::request& req) {
        auto user = current_user(*db, req);
        if (!user) return crow::response(401, "not logged in");

        std::string query;
        if (auto q = req.url_params.get("q")) query = q;

        try {
            std::string access_token = mint_access_token(cfg, *db, user->id);
            std::vector<oauth::SheetFile> files = oauth::list_spreadsheets(access_token, query);

            std::string body = "{\"files\":[";
            for (size_t i = 0; i < files.size(); ++i) {
                if (i > 0) body += ",";
                body += "{\"id\":\"" + json_escape(files[i].id) + "\",";
                body += "\"name\":\"" + json_escape(files[i].name) + "\",";
                body += "\"modifiedTime\":\"" + json_escape(files[i].modified_time) + "\"}";
            }
            body += "]}";

            crow::response res(200, body);
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            std::cerr << "sheets/search failed for user " << user->id << ": " << e.what()
                       << std::endl;
            return crow::response(502, "could not search Google Drive, please try again");
        }
    });

    // List sheets the user has already saved (the picker's "your sheets").
    CROW_ROUTE(app, "/sheets")([&db](const crow::request& req) {
        auto user = current_user(*db, req);
        if (!user) return crow::response(401, "not logged in");

        std::vector<Sheet> sheets = db->list_sheets(user->id);
        std::string body = "{\"sheets\":[";
        for (size_t i = 0; i < sheets.size(); ++i) {
            if (i > 0) body += ",";
            body += "{\"row_id\":" + std::to_string(sheets[i].id) + ",";
            body += "\"sheet_id\":\"" + json_escape(sheets[i].sheet_id) + "\",";
            body += "\"display_name\":\"" + json_escape(sheets[i].display_name) + "\"}";
        }
        body += "]}";

        crow::response res(200, body);
        res.set_header("Content-Type", "application/json");
        return res;
    });

    // Save a sheet the user picked from /sheets/search results. Body:
    // {"sheet_id": "...", "display_name": "..."}. We deliberately don't
    // re-verify the sheet_id against Drive here — the Sheets-read call at
    // generate time will fail cleanly if it's bogus or access was revoked,
    // and re-checking on every save just doubles the Google round trips.
    CROW_ROUTE(app, "/sheets").methods(crow::HTTPMethod::Post)(
        [&db](const crow::request& req) {
            auto user = current_user(*db, req);
            if (!user) return crow::response(401, "not logged in");

            auto json = crow::json::load(req.body);
            if (!json || !json.has("sheet_id") || !json.has("display_name")) {
                return crow::response(400, "expected {\"sheet_id\":..., \"display_name\":...}");
            }
            std::string sheet_id = json["sheet_id"].s();
            std::string display_name = json["display_name"].s();
            if (sheet_id.empty() || display_name.empty()) {
                return crow::response(400, "sheet_id and display_name must be non-empty");
            }

            try {
                Sheet saved = db->add_sheet(user->id, sheet_id, display_name);
                std::string body = "{\"row_id\":" + std::to_string(saved.id) + ",";
                body += "\"sheet_id\":\"" + json_escape(saved.sheet_id) + "\",";
                body += "\"display_name\":\"" + json_escape(saved.display_name) + "\"}";
                crow::response res(200, body);
                res.set_header("Content-Type", "application/json");
                return res;
            } catch (const std::exception& e) {
                std::cerr << "sheets add failed for user " << user->id << ": " << e.what()
                           << std::endl;
                return crow::response(500, "could not save sheet");
            }
        });

    CROW_ROUTE(app, "/sheets/<int>").methods(crow::HTTPMethod::Delete)(
        [&db](const crow::request& req, int64_t row_id) {
            auto user = current_user(*db, req);
            if (!user) return crow::response(401, "not logged in");

            bool removed = db->delete_sheet(user->id, row_id);
            return crow::response(removed ? 200 : 404, removed ? "deleted" : "not found");
        });

    // Generates labels.pdf for a saved sheet, synchronously — fetches the
    // live sheet, pivots it, renders the PDF, and returns it as a
    // download. Nothing is written to disk except the shared (non-
    // sensitive) LEGO element photo cache; the PDF itself only ever exists
    // in memory + the HTTP response.
    CROW_ROUTE(app, "/sheets/<int>/labels").methods(crow::HTTPMethod::Post)(
        [&cfg, &db](const crow::request& req, int64_t row_id) {
            auto user = current_user(*db, req);
            if (!user) return crow::response(401, "not logged in");

            auto owned = db->find_owned_sheet(user->id, row_id);
            if (!owned) return crow::response(404, "sheet not found");

            try {
                PivotResult pivot = fetch_and_pivot(cfg, *db, user->id, owned->sheet_id);
                std::vector<uint8_t> pdf =
                    labels_pdf::build_labels_pdf(pivot.records, cfg.data_dir + "/image_cache");

                db->log_run(owned->sheet_row_id, "labels",
                            static_cast<int>(pivot.records.size()), "ok", nullptr);

                std::string filename = safe_filename_stem(owned->display_name) + " labels.pdf";
                crow::response res(200);
                res.set_header("Content-Type", "application/pdf");
                res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
                res.body.assign(reinterpret_cast<const char*>(pdf.data()), pdf.size());
                return res;
            } catch (const std::exception& e) {
                std::cerr << "labels generation failed for sheet " << owned->sheet_row_id << ": "
                           << e.what() << std::endl;
                std::string err = "generation failed";
                db->log_run(owned->sheet_row_id, "labels", 0, "error", &err);
                return crow::response(502, "could not generate labels — check the sheet is "
                                            "still shared with your Google account and try again");
            }
        });

    // Generates the lot-count report for a saved sheet: ?format=csv
    // (default) or ?format=pdf.
    CROW_ROUTE(app, "/sheets/<int>/lots").methods(crow::HTTPMethod::Post)(
        [&cfg, &db](const crow::request& req, int64_t row_id) {
            auto user = current_user(*db, req);
            if (!user) return crow::response(401, "not logged in");

            auto owned = db->find_owned_sheet(user->id, row_id);
            if (!owned) return crow::response(404, "sheet not found");

            std::string format = "csv";
            if (auto f = req.url_params.get("format")) format = f;
            if (format != "csv" && format != "pdf") {
                return crow::response(400, "format must be 'csv' or 'pdf'");
            }

            try {
                PivotResult pivot = fetch_and_pivot(cfg, *db, user->id, owned->sheet_id);
                auto totals = reports::lot_counts_by_person(pivot.records, reports::SortBy::kLastName);

                std::string stem = safe_filename_stem(owned->display_name) + " lot counts";
                crow::response res(200);
                if (format == "csv") {
                    res.set_header("Content-Type", "text/csv; charset=utf-8");
                    res.set_header("Content-Disposition", "attachment; filename=\"" + stem + ".csv\"");
                    res.body = reports::lot_counts_csv(pivot.records, reports::SortBy::kLastName);
                } else {
                    std::vector<uint8_t> pdf =
                        reports::lot_counts_pdf(pivot.records, reports::SortBy::kLastName);
                    res.set_header("Content-Type", "application/pdf");
                    res.set_header("Content-Disposition", "attachment; filename=\"" + stem + ".pdf\"");
                    res.body.assign(reinterpret_cast<const char*>(pdf.data()), pdf.size());
                }

                db->log_run(owned->sheet_row_id, "lot_counts", static_cast<int>(totals.size()),
                            "ok", nullptr);
                return res;
            } catch (const std::exception& e) {
                std::cerr << "lot_counts generation failed for sheet " << owned->sheet_row_id
                           << ": " << e.what() << std::endl;
                std::string err = "generation failed";
                db->log_run(owned->sheet_row_id, "lot_counts", 0, "error", &err);
                return crow::response(502, "could not generate lot counts — check the sheet is "
                                            "still shared with your Google account and try again");
            }
        });

    app.port(8080).multithreaded().run();
}
