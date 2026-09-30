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
//   GET    /sheets/:id/check    pivot the sheet and report data issues (JSON)
//   POST   /sheets/:id/labels   generate the label PDF (?spec=avery5162&order=heaviest)
//   POST   /sheets/:id/lots     lot counts per person, ?format=csv (default) or pdf
//   POST   /sheets/:id/parts    parts list (pieces + people per part), ?format=csv|pdf&order=
//   GET    /sheets/:id/history  recent generate runs for a sheet (JSON)
//
// See sql/schema.sql for the users/sheets/runs/sessions tables.

#include "crow.h"
#include "crow/json.h"
#include "crow/mustache.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <vector>

#include "bricklink.h"
#include "colors.h"
#include "config.h"
#include "crypto.h"
#include "db.h"
#include "labels_pdf.h"
#include "oauth.h"
#include "ordering.h"
#include "reports.h"
#include "sheet_layout.h"
#include "sheet_pivot.h"

namespace {

using namespace lugbulk;

constexpr int kSessionTtlSeconds = 30 * 24 * 60 * 60;  // 30 days
constexpr int kStateTtlSeconds = 10 * 60;              // OAuth round trip window
constexpr const char* kSessionCookie = "lugbulk_session";
constexpr const char* kStateCookie = "lugbulk_oauth_state";
constexpr size_t kMaxDisplayNameLen = 200;

// "https://host[:port]" of the app, from the OAuth redirect URI (the one
// place the public origin is configured).
std::string app_origin(const Config& cfg) {
    const std::string& uri = cfg.google_redirect_uri;
    size_t scheme_end = uri.find("://");
    if (scheme_end == std::string::npos) return "";
    size_t path_start = uri.find('/', scheme_end + 3);
    return uri.substr(0, path_start);
}

// Applied to every response:
//  - Security headers, and a text/plain default so an error message that
//    echoes request text can never be sniffed as HTML.
//  - A same-origin check on state-changing requests. SameSite=Lax cookies
//    already keep cross-site POSTs unauthenticated; this is belt and braces
//    for older browsers.
struct SecurityMiddleware {
    struct context {};
    std::string origin;  // set in main() from the config

    void before_handle(crow::request& req, crow::response& res, context&) {
        if (req.method == crow::HTTPMethod::Get || req.method == crow::HTTPMethod::Head) return;
        std::string req_origin = req.get_header_value("Origin");
        if (!req_origin.empty() && !origin.empty() && req_origin != origin) {
            res.code = 403;
            res.body = "cross-origin request refused";
            res.end();
        }
    }

    void after_handle(crow::request&, crow::response& res, context&) {
        if (res.get_header_value("Content-Type").empty()) {
            res.set_header("Content-Type", "text/plain; charset=utf-8");
        }
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Referrer-Policy", "same-origin");
        res.set_header("Content-Security-Policy",
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                       "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                       "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    }
};

using App = crow::App<SecurityMiddleware>;

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

// The user's stored Google authorization is gone or unusable (never stored,
// undecryptable after a key change, or revoked on Google's side) — they
// need to log in again, which re-consents and stores a fresh token.
struct ReauthRequired : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Decrypts the user's stored refresh token and exchanges it for a fresh,
// short-lived access token. The access token is returned to the caller for
// one immediate outbound Google API call and is never written to the DB or
// logs; the decrypted refresh token similarly never leaves this function's
// stack. Throws ReauthRequired if the stored refresh token is missing,
// undecryptable, or rejected by Google; std::runtime_error on transport
// failures.
std::string mint_access_token(const Config& cfg, Db& db, int64_t user_id) {
    auto enc = db.get_refresh_token_enc(user_id);
    if (!enc || enc->empty()) {
        throw ReauthRequired("no stored Google refresh token for this user");
    }
    std::string refresh_token;
    try {
        refresh_token = crypto::aes_gcm_decrypt(cfg.token_encryption_key_b64, *enc);
    } catch (const std::runtime_error& e) {
        throw ReauthRequired(std::string("stored refresh token unusable: ") + e.what());
    }
    try {
        return oauth::refresh_access_token(cfg, refresh_token).access_token;
    } catch (const oauth::HttpError& e) {
        // 400 invalid_grant: revoked or expired on Google's side.
        throw ReauthRequired(std::string("refresh grant rejected: ") + e.what());
    }
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

// Looks every part up on BrickLink (cached in the DB; a no-op without
// credentials): sets each record's catalog_weight, and fills in color
// names the sheet left blank. Never fails the request — an API problem
// just means estimated weights, and is logged.
void apply_bricklink(const Config& cfg, Db& db, PivotResult& pivot) {
    constexpr int64_t kMissRetrySeconds = 7 * 24 * 60 * 60;
    constexpr size_t kWorkers = 4;
    if (!cfg.bricklink.complete() || pivot.records.empty()) return;

    std::vector<std::string> ids;
    for (const auto& r : pivot.records) ids.push_back(r.element_id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    std::map<std::string, BrickLinkPart> parts = db.get_bricklink_parts(ids);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::vector<std::string> todo;
    for (const auto& id : ids) {
        auto it = parts.find(id);
        bool fresh = it != parts.end() &&
                     (it->second.weight || now - it->second.fetched_at < kMissRetrySeconds);
        if (!fresh) todo.push_back(id);
    }

    if (!todo.empty()) {
        std::mutex mu;
        std::atomic<size_t> next{0};
        std::atomic<bool> api_refused{false};
        auto worker = [&] {
            for (size_t i; !api_refused && (i = next.fetch_add(1)) < todo.size();) {
                try {
                    bricklink::PartInfo info = bricklink::fetch(todo[i], cfg.bricklink);
                    BrickLinkPart p{todo[i], info.part_no, info.color, info.weight, now};
                    std::lock_guard<std::mutex> lock(mu);
                    db.put_bricklink_part(p);
                    parts[todo[i]] = std::move(p);
                } catch (const bricklink::ApiError& e) {
                    if (!api_refused.exchange(true)) std::cerr << e.what() << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "bricklink lookup of " << todo[i] << " failed: " << e.what()
                              << std::endl;  // network blip: stays uncached, retried next time
                }
            }
        };
        std::vector<std::future<void>> futures;
        for (size_t i = 0; i < std::min(kWorkers, todo.size()); ++i) {
            futures.push_back(std::async(std::launch::async, worker));
        }
        for (auto& f : futures) f.wait();
    }

    std::set<std::string> colored;
    for (auto& r : pivot.records) {
        auto it = parts.find(r.element_id);
        if (it == parts.end()) continue;
        r.catalog_weight = it->second.weight;
        if (r.bl_color.empty() && !it->second.color.empty()) {
            r.bl_color = it->second.color;
            if (r.lego_color.empty()) r.lego_color = colors::resolve("", r.bl_color).lego;
            colored.insert(r.element_id);
        }
    }
    // A color BrickLink supplied is no longer missing.
    std::erase_if(pivot.issues, [&](const SheetIssue& i) {
        return i.kind == "missing_color" && colored.count(i.element_id);
    });
}

// Fetches the "Order Here" tab for a sheet the user owns, pivots it, and
// adds BrickLink data. Shared by every generate route. Throws std::runtime_error (from
// mint_access_token / oauth calls) on any Google API failure — callers turn
// that into a run-log "error" row + an error response (see google_error).
PivotResult fetch_and_pivot(const Config& cfg, Db& db, int64_t user_id,
                             const std::string& spreadsheet_id) {
    std::string access_token = mint_access_token(cfg, db, user_id);
    // Column count has grown across sheet years (2023: 93 cols -> 2026: 98
    // cols, as the roster grows) — ZZ (702 columns) gives a wide margin
    // against a fixed cutoff silently truncating future, larger rosters.
    std::string range = "'" + std::string(layout::kSourceTab) + "'!A1:ZZ";
    std::vector<std::vector<std::string>> rows =
        oauth::fetch_sheet_values(access_token, spreadsheet_id, range);
    PivotResult pivot = pivot_sheet(rows);
    apply_bricklink(cfg, db, pivot);
    return pivot;
}

// Turns a failed Google call into a response the organizer can act on.
crow::response google_error(const std::exception& e) {
    if (dynamic_cast<const ReauthRequired*>(&e)) {
        return crow::response(401, "Your Google access has expired or was revoked — log out "
                                   "and log back in.");
    }
    if (auto* http = dynamic_cast<const oauth::HttpError*>(&e)) {
        switch (http->status) {
            case 400:
                return crow::response(502, std::string("Google couldn't read the '") +
                                               layout::kSourceTab +
                                               "' tab — check the sheet has a tab by that name.");
            case 403:
            case 404:
                return crow::response(502, "Google says this sheet doesn't exist or isn't shared "
                                           "with your Google account.");
            default:
                break;
        }
    }
    return crow::response(502, "Couldn't reach Google Sheets — try again in a minute.");
}

std::string sheet_error_json(const PivotResult& pivot) {
    crow::json::wvalue body;
    body["labels"] = pivot.records.size();
    std::vector<crow::json::wvalue> issues;
    for (const auto& issue : pivot.issues) {
        crow::json::wvalue item;
        item["row"] = issue.row;
        item["kind"] = issue.kind;
        item["detail"] = issue.detail;
        issues.push_back(std::move(item));
    }
    body["issues"] = std::move(issues);
    return body.dump();
}

// Drive/Sheets file ids are URL-safe base64-ish; reject anything else
// before it's stored or put in an API URL.
bool valid_sheet_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

crow::response attachment(const std::string& content_type, const std::string& filename,
                          std::string body) {
    crow::response res(200);
    res.set_header("Content-Type", content_type);
    res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    res.body = std::move(body);
    return res;
}

std::string as_string(const std::vector<uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
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

    try {
        // Relative to the working directory, like sql/ and templates/.
        layout::load_label_specs(std::getenv("LUGBULK_LABEL_SPECS") ? std::getenv("LUGBULK_LABEL_SPECS")
                                                                     : "data/label_specs.json");
    } catch (const std::exception& e) {
        std::cerr << "startup failed: " << e.what() << std::endl;
        return 1;
    }

    // Clear out sessions that expired while the server was down.
    db->delete_expired_sessions();

    App app;
    app.get_middleware<SecurityMiddleware>().origin = app_origin(cfg);
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
        // Label stock picker: one <optgroup> per brand + page size.
        std::vector<crow::json::wvalue> groups;
        std::vector<crow::json::wvalue> options;
        std::string group_name;
        auto flush = [&] {
            if (options.empty()) return;
            crow::json::wvalue g;
            g["label"] = group_name;
            g["specs"] = std::move(options);
            groups.push_back(std::move(g));
            options.clear();
        };
        for (const auto& spec : layout::label_specs()) {
            std::string name = spec.brand + (spec.page == "roll" ? " LabelWriter rolls"
                                                                 : " " + spec.page + " sheets");
            if (name != group_name) {
                flush();
                group_name = name;
            }
            crow::json::wvalue item;
            item["id"] = spec.id;
            std::string label = spec.display_name();
            if (!spec.equivalents.empty()) {
                label += " (also";
                for (const auto& e : spec.equivalents) label += " " + e;
                label += ")";
            }
            item["name"] = label;
            if (spec.id == layout::kDefaultLabelSpecId) item["selected"] = true;
            options.push_back(std::move(item));
        }
        flush();
        ctx["spec_groups"] = std::move(groups);
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
        if (req.url_params.get("error")) {
            // User declined consent, or Google reported a problem. Not
            // echoed back: it's attacker-controllable query text.
            return crow::response(400, "Google sign-in was cancelled or failed — try again.");
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

            db->delete_expired_sessions();
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
        } catch (const ReauthRequired& e) {
            std::cerr << "sheets/search needs re-login for user " << user->id << ": " << e.what()
                      << std::endl;
            return google_error(e);
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
            if (!valid_sheet_id(sheet_id)) {
                return crow::response(400, "sheet_id doesn't look like a Google Sheets id");
            }
            if (display_name.empty() || display_name.size() > kMaxDisplayNameLen) {
                return crow::response(400, "display_name must be 1-200 characters");
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

    // Pivots the live sheet and reports what a generate would produce:
    // label count plus any data issues (bad quantities, duplicates, colors
    // with no LEGO/BrickLink match...), so they can be fixed first.
    CROW_ROUTE(app, "/sheets/<int>/check")([&cfg, &db](const crow::request& req, int64_t row_id) {
        auto user = current_user(*db, req);
        if (!user) return crow::response(401, "not logged in");
        auto owned = db->find_owned_sheet(user->id, row_id);
        if (!owned) return crow::response(404, "sheet not found");

        try {
            PivotResult pivot = fetch_and_pivot(cfg, *db, user->id, owned->sheet_id);
            crow::response res(200, sheet_error_json(pivot));
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            std::cerr << "check failed for sheet " << owned->sheet_row_id << ": " << e.what()
                      << std::endl;
            return google_error(e);
        }
    });

    CROW_ROUTE(app, "/sheets/<int>/history")([&db](const crow::request& req, int64_t row_id) {
        auto user = current_user(*db, req);
        if (!user) return crow::response(401, "not logged in");
        auto owned = db->find_owned_sheet(user->id, row_id);
        if (!owned) return crow::response(404, "sheet not found");

        std::vector<crow::json::wvalue> runs;
        for (const auto& run : db->list_runs(owned->sheet_row_id, 10)) {
            crow::json::wvalue item;
            item["report_type"] = run.report_type;
            item["generated_at"] = run.generated_at;
            item["item_count"] = run.item_count;
            item["status"] = run.status;
            item["error"] = run.error_message;
            runs.push_back(std::move(item));
        }
        crow::json::wvalue body;
        body["runs"] = std::move(runs);
        crow::response res(200, body.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    });

    // Every generate route: owner check, fetch + pivot, render, log the
    // run. `render` gets the pivot result and returns the response (or
    // throws). Synchronous — the file only ever exists in memory and the
    // HTTP response; nothing is written to disk except the shared (non-
    // sensitive) LEGO element photo cache.
    auto generate = [&cfg, &db](const crow::request& req, int64_t row_id,
                                const std::string& report_type, auto render) {
        auto user = current_user(*db, req);
        if (!user) return crow::response(401, "not logged in");
        auto owned = db->find_owned_sheet(user->id, row_id);
        if (!owned) return crow::response(404, "sheet not found");

        PivotResult pivot;
        try {
            pivot = fetch_and_pivot(cfg, *db, user->id, owned->sheet_id);
        } catch (const std::exception& e) {
            std::cerr << report_type << " fetch failed for sheet " << owned->sheet_row_id << ": "
                      << e.what() << std::endl;
            std::string err = "google fetch failed";
            db->log_run(owned->sheet_row_id, report_type, 0, "error", &err);
            return google_error(e);
        }
        if (pivot.records.empty()) {
            std::string err = "no label records";
            db->log_run(owned->sheet_row_id, report_type, 0, "error", &err);
            return crow::response(422, std::string("No orders found on the '") +
                                           layout::kSourceTab +
                                           "' tab — use Check sheet to see why.");
        }

        try {
            auto [res, item_count] = render(pivot, owned->display_name);
            db->log_run(owned->sheet_row_id, report_type, item_count, "ok", nullptr);
            return std::move(res);
        } catch (const std::exception& e) {
            std::cerr << report_type << " generation failed for sheet " << owned->sheet_row_id
                      << ": " << e.what() << std::endl;
            std::string err = "generation failed";
            db->log_run(owned->sheet_row_id, report_type, 0, "error", &err);
            return crow::response(500, "Couldn't build the file — try again.");
        }
    };

    auto part_order_param = [](const crow::request& req) {
        const char* order = req.url_params.get("order");
        return ordering::parse_part_order(order ? order : "heaviest");
    };

    CROW_ROUTE(app, "/sheets/<int>/labels").methods(crow::HTTPMethod::Post)(
        [&](const crow::request& req, int64_t row_id) {
            const char* spec_id = req.url_params.get("spec");
            const layout::LabelSpec* spec =
                spec_id ? layout::find_label_spec(spec_id) : &layout::default_label_spec();
            auto order = part_order_param(req);
            if (!spec) return crow::response(400, "unknown label spec");
            if (!order) return crow::response(400, "order must be heaviest, lightest or sheet");

            return generate(req, row_id, "labels", [&](PivotResult& pivot, const std::string& name) {
                auto records = ordering::order_records(std::move(pivot.records), *order);
                std::vector<uint8_t> pdf =
                    labels_pdf::build_labels_pdf(records, cfg.data_dir + "/image_cache", *spec);
                return std::make_pair(
                    attachment("application/pdf", safe_filename_stem(name) + " labels.pdf",
                               as_string(pdf)),
                    static_cast<int>(records.size()));
            });
        });

    auto format_param = [](const crow::request& req) -> std::optional<std::string> {
        const char* f = req.url_params.get("format");
        std::string format = f ? f : "csv";
        if (format != "csv" && format != "pdf") return std::nullopt;
        return format;
    };

    CROW_ROUTE(app, "/sheets/<int>/lots").methods(crow::HTTPMethod::Post)(
        [&](const crow::request& req, int64_t row_id) {
            auto format = format_param(req);
            if (!format) return crow::response(400, "format must be 'csv' or 'pdf'");

            return generate(req, row_id, "lot_counts",
                            [&](PivotResult& pivot, const std::string& name) {
                const auto sort = reports::SortBy::kLastName;
                int people = static_cast<int>(reports::lot_counts_by_person(pivot.records, sort).size());
                std::string stem = safe_filename_stem(name) + " lot counts";
                if (*format == "csv") {
                    return std::make_pair(attachment("text/csv; charset=utf-8", stem + ".csv",
                                                     reports::lot_counts_csv(pivot.records, sort)),
                                          people);
                }
                return std::make_pair(
                    attachment("application/pdf", stem + ".pdf",
                               as_string(reports::lot_counts_pdf(pivot.records, sort))),
                    people);
            });
        });

    CROW_ROUTE(app, "/sheets/<int>/parts").methods(crow::HTTPMethod::Post)(
        [&](const crow::request& req, int64_t row_id) {
            auto format = format_param(req);
            auto order = part_order_param(req);
            if (!format) return crow::response(400, "format must be 'csv' or 'pdf'");
            if (!order) return crow::response(400, "order must be heaviest, lightest or sheet");

            return generate(req, row_id, "parts", [&](PivotResult& pivot, const std::string& name) {
                auto parts = ordering::summarize_parts(pivot.records, *order);
                int count = static_cast<int>(parts.size());
                std::string stem = safe_filename_stem(name) + " parts";
                if (*format == "csv") {
                    return std::make_pair(
                        attachment("text/csv; charset=utf-8", stem + ".csv", reports::parts_csv(parts)),
                        count);
                }
                return std::make_pair(
                    attachment("application/pdf", stem + ".pdf", as_string(reports::parts_pdf(parts))),
                    count);
            });
        });

    app.port(8080).multithreaded().run();
}
