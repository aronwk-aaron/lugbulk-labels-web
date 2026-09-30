// lugbulk-labels-web — hosted counterpart to the lugbulk-label CLI.
//
// Routes so far:
//   GET    /                    dashboard HTML, mustache-rendered (redirects to /auth/login if not logged in)
//   GET    /healthz             liveness check
//   GET    /version             build version (release, canary-<sha>, or dev)
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
//   POST   /sheets/:id/checklist  packing checklist PDF
//   POST   /sheets/:id/all      every report in one .zip ("Download all")
//   GET    /sheets/:id/design   the sheet's saved label design (JSON)
//   PUT    /sheets/:id/design   save it: {"spec","order","hide"}
//   GET    /preview             one page of sample labels (?spec=&order=&hide=)
//   GET    /test-page           printer alignment page for a stock (?spec=)
//
// See sql/schema.sql for the users/sheets/runs/sessions tables.

#include "crow.h"
#include "crow/json.h"
#include "crow/mustache.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <cctype>
#include <ctime>
#include <functional>
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
#include "rate_limits.h"
#include "samples.h"
#include "spreadsheet.h"
#include "zip_writer.h"
#include "oauth.h"
#include "ordering.h"
#include "reports.h"
#include "sheet_layout.h"
#include "sheet_pivot.h"

namespace {

using namespace lugbulk;

// Sign-ins last two weeks: long enough for event prep, short enough that a
// forgotten session on a shared computer doesn't stay open for a month.
constexpr int kSessionTtlSeconds = 14 * 24 * 60 * 60;
constexpr int kStateTtlSeconds = 10 * 60;              // OAuth round trip window
constexpr const char* kSessionCookie = "lugbulk_session";
constexpr const char* kStateCookie = "lugbulk_oauth_state";
constexpr size_t kMaxDisplayNameLen = 200;

// A same-site redirect target as an absolute URL on the app's public
// origin. Crow turns a relative Location into an absolute one using its
// own (plain http) connection, which is wrong behind an https proxy —
// giving it a full URL makes it leave the header alone.
std::string app_origin(const Config& cfg);
std::string local_url(const Config& cfg, const std::string& path) {
    std::string origin = app_origin(cfg);
    return origin.empty() ? path : origin + path;
}

// "https://host[:port]" of the app, from the OAuth redirect URI (the one
// place the public origin is configured).
std::string app_origin(const Config& cfg) { return cfg.public_url; }

// Applied to every response:
//  - Security headers, and a text/plain default so an error message that
//    echoes request text can never be sniffed as HTML.
//  - A same-origin check on state-changing requests. SameSite=Lax cookies
//    already keep cross-site POSTs unauthenticated; this is belt and braces
//    for older browsers.
// Size limits on what one request may make the server do.
constexpr int kMaxSheetRows = 3000;       // rows fetched from the "Order Here" tab
constexpr size_t kMaxLabels = 20000;      // labels in one run
constexpr size_t kMaxParts = 2000;        // distinct parts in one run (photo downloads)
constexpr size_t kMaxSavedSheets = 50;    // per user
constexpr int kMaxSessionsPerUser = 10;

// Rate limits and caps shared by every request (see rate_limits.h). Built once
// in main() from the config.
struct Guards {
    // Any request, per client IP: bursts of 120 (a dashboard load is one
    // request per saved sheet), then 10/second.
    limits::RateLimiter per_ip{120, 10};
    // Sign-in routes, per client IP: 10, then one per 6 seconds.
    limits::RateLimiter auth_per_ip{10, 1.0 / 6};
    // Drive searches, per user: 10, then one per 3 seconds.
    limits::RateLimiter search_per_user{10, 1.0 / 3};
    // Reports (and Check sheet), per user: 6, then one per 2 minutes.
    limits::RateLimiter jobs_per_user{6, 1.0 / 120};
    // Live design preview and alignment test page, per user: 20, then 1/second.
    limits::RateLimiter preview_per_user{20, 1};
    limits::JobGate jobs;
    limits::Allowlist allowlist;
    bool trust_proxy;

    explicit Guards(const Config& cfg)
        : jobs(cfg.max_concurrent_jobs),
          allowlist(cfg.allowed_emails),
          trust_proxy(cfg.trust_proxy) {}
};
Guards* g_guards = nullptr;  // set in main() before the server starts

// The client's IP: the socket peer, or with TRUST_PROXY the first address
// in X-Forwarded-For (the proxy is then the socket peer).
std::string client_ip(const crow::request& req) {
    if (g_guards && g_guards->trust_proxy) {
        std::string xff = req.get_header_value("X-Forwarded-For");
        if (!xff.empty()) {
            std::string first = xff.substr(0, xff.find(','));
            first.erase(0, first.find_first_not_of(' '));
            first.erase(first.find_last_not_of(' ') + 1);
            if (!first.empty()) return first;
        }
    }
    return req.remote_ip_address;
}

crow::response too_many(int retry_after, const std::string& message) {
    crow::response res(429, message);
    res.set_header("Retry-After", std::to_string(std::max(1, retry_after)));
    return res;
}

// Rate-limit key for a request: the signed-in user, else the client IP.
std::string visitor_key(const crow::request& req, const std::optional<User>& user) {
    return user ? "user:" + std::to_string(user->id) : "ip:" + client_ip(req);
}

// Admission for an expensive request (a report or Check sheet): the
// user's rate limit, then one job per user and MAX_CONCURRENT_JOBS
// server-wide. Refusals are immediate — nothing queues on a worker thread.
struct JobAdmission {
    std::optional<limits::JobGate::Ticket> ticket;
    crow::response refused;
};

JobAdmission start_job(const std::string& limiter_key, int64_t gate_id);

JobAdmission start_job(int64_t user_id) {
    return start_job("user:" + std::to_string(user_id), user_id);
}

// Same, for an anonymous upload: limits keyed by client IP.
JobAdmission start_anonymous_job(const std::string& ip) {
    // A negative gate id per IP, so it can't collide with a user id.
    int64_t gate_id = -1 - static_cast<int64_t>(std::hash<std::string>{}(ip) & 0x3fffffffffffffffULL);
    return start_job("ip:" + ip, gate_id);
}

JobAdmission start_job(const std::string& limiter_key, int64_t gate_id) {
    JobAdmission a;
    if (auto retry = g_guards->jobs_per_user.take(limiter_key)) {
        a.refused = too_many(*retry, "That's a lot of reports in a short time — try again in " +
                                         std::to_string(*retry) + " seconds.");
        return a;
    }
    auto result = g_guards->jobs.enter(gate_id);
    if (!result.ticket) {
        a.refused = result.refusal == limits::JobGate::Refusal::kUserBusy
                        ? too_many(5, "You already have a report running — wait for it to finish.")
                        : too_many(10, "The server is busy with other reports — try again in a "
                                       "few seconds.");
        return a;
    }
    a.ticket.emplace(std::move(*result.ticket));
    return a;
}

// Content-Security-Policy. Scripts only run if they carry `nonce` (the
// dashboard's own inline scripts); with no nonce, no script runs.
std::string csp_for(const std::string& nonce) {
    std::string script = nonce.empty() ? "'none'" : "'nonce-" + nonce + "'";
    return "default-src 'self'; script-src " + script + "; style-src 'self' 'unsafe-inline'; "
           "img-src 'self' data:; frame-src blob:; object-src 'none'; frame-ancestors 'none'; "
           "base-uri 'none'; form-action 'self'";
}

// Whether a request's Origin header is this app. Compared to the configured
// public URL, or without one to the Host the request was sent to.
bool same_origin(const std::string& req_origin, const std::string& public_url,
                 const std::string& host) {
    if (!public_url.empty()) return req_origin == public_url;
    size_t scheme_end = req_origin.find("://");
    return scheme_end != std::string::npos && req_origin.substr(scheme_end + 3) == host;
}

struct SecurityMiddleware {
    struct context {};
    std::string origin;  // set in main() from the config
    bool https = false;  // served over https (per the OAuth redirect URI)

    void before_handle(crow::request& req, crow::response& res, context&) {
        if (g_guards && req.url != "/healthz") {
            std::string ip = client_ip(req);
            auto retry = g_guards->per_ip.take(ip);
            if (!retry && req.url.rfind("/auth/", 0) == 0) retry = g_guards->auth_per_ip.take(ip);
            if (retry) {
                res = too_many(*retry, "Too many requests — slow down and try again shortly.");
                res.end();
                return;
            }
        }
        if (req.method == crow::HTTPMethod::Get || req.method == crow::HTTPMethod::Head) return;
        std::string req_origin = req.get_header_value("Origin");
        if (!req_origin.empty() && !same_origin(req_origin, origin, req.get_header_value("Host"))) {
            res.code = 403;
            res.body = "cross-origin request refused";
            res.end();
        }
    }

    void after_handle(crow::request&, crow::response& res, context&) {
        if (res.get_header_value("Content-Type").empty()) {
            res.set_header("Content-Type", "text/plain; charset=utf-8");
        }
        // Nearly every response is one person's data (their sheets, labels
        // with names on them). Never let a browser, proxy or CDN cache it —
        // otherwise a shared computer's Back button, or a caching proxy,
        // could show one organizer's data to someone else.
        res.set_header("Cache-Control", "no-store, private");
        res.set_header("Pragma", "no-cache");
        res.set_header("Vary", "Cookie");
        res.set_header("Server", "lugbulk-labels-web");  // don't advertise the framework
        if (https) res.set_header("Strict-Transport-Security", "max-age=31536000");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Referrer-Policy", "same-origin");
        // The dashboard sets its own CSP with a per-response script nonce;
        // everything else gets this one, which allows no scripts at all.
        if (res.get_header_value("Content-Security-Policy").empty()) {
            res.set_header("Content-Security-Policy", csp_for(""));
        }
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
    if (cfg.https()) {
        attrs += "; Secure";
    }
    return attrs;
}

std::string clear_cookie_attrs(const Config& cfg) {
    std::string attrs = "Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
    if (cfg.https()) {
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

bool g_google_enabled = false;  // set in main()

std::optional<User> current_user(Db& db, const crow::request& req) {
    if (!g_google_enabled) return std::nullopt;  // no sign-in: everyone is anonymous
    auto token = get_cookie(req, kSessionCookie);
    if (!token) return std::nullopt;
    auto user = db.find_user_by_session(*token);
    // Taking someone off ALLOWED_EMAILS locks them out at once, not when
    // their session expires.
    if (user && g_guards && !g_guards->allowlist.allows(user->email)) return std::nullopt;
    return user;
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

// Fills in BrickLink data from the catalog files (see bricklink.h): each
// record's catalog_weight, and BrickLink/LEGO color names the sheet left
// blank. A no-op when the files aren't there.
bricklink::CatalogCache* g_catalog = nullptr;  // set in main()

void apply_bricklink(PivotResult& pivot) {
    if (!g_catalog || pivot.records.empty()) return;
    std::shared_ptr<const bricklink::Catalog> catalog = g_catalog->get();
    if (catalog->empty()) return;
    std::set<std::string> colored;
    for (auto& r : pivot.records) {
        auto it = catalog->find(r.element_id);
        if (it == catalog->end()) continue;
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

// A sheet over the per-run size limits.
struct TooBig : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void check_run_size(const PivotResult& pivot) {
    std::set<std::string> parts;
    for (const auto& r : pivot.records) parts.insert(r.element_id);
    if (pivot.records.size() > kMaxLabels || parts.size() > kMaxParts) {
        throw TooBig("this sheet has " + std::to_string(pivot.records.size()) + " labels and " +
                     std::to_string(parts.size()) + " parts; the limit is " +
                     std::to_string(kMaxLabels) + " labels / " + std::to_string(kMaxParts) +
                     " parts per run");
    }
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
    // Rows are capped too: a real order sheet is ~100 rows, and a sheet
    // of millions shouldn't be able to exhaust the server's memory.
    std::string range = "'" + std::string(layout::kSourceTab) + "'!A1:ZZ" +
                        std::to_string(kMaxSheetRows);
    std::vector<std::vector<std::string>> rows =
        oauth::fetch_sheet_values(access_token, spreadsheet_id, range);
    PivotResult pivot = pivot_sheet(rows);
    check_run_size(pivot);
    apply_bricklink(pivot);
    return pivot;
}

// Pivots an uploaded .xlsx or .csv (see spreadsheet.h). For a workbook,
// uses the "Order Here" tab, or failing that the first tab with orders on
// it. Throws spreadsheet::Error (bad file) or TooBig.
PivotResult pivot_upload(const std::string& data) {
    PivotResult pivot;
    if (spreadsheet::is_xlsx(data)) {
        for (auto& rows : spreadsheet::read_xlsx(data, layout::kSourceTab)) {
            if (rows.size() > static_cast<size_t>(kMaxSheetRows)) rows.resize(kMaxSheetRows);
            pivot = pivot_sheet(rows);
            if (!pivot.records.empty()) break;
        }
    } else {
        auto rows = spreadsheet::read_csv(data);
        if (rows.size() > static_cast<size_t>(kMaxSheetRows)) rows.resize(kMaxSheetRows);
        pivot = pivot_sheet(rows);
    }
    check_run_size(pivot);
    apply_bricklink(pivot);
    return pivot;
}

// The label design for a request: the sheet's saved design (or the
// defaults), with any spec/order/hide query parameters on top.
struct EffectiveDesign {
    const layout::LabelSpec* spec;
    ordering::PartOrder order;
    labels_pdf::LabelOptions options;
};

std::optional<EffectiveDesign> resolve_design(const std::optional<Design>& saved,
                                              const crow::request& req, std::string* error) {
    auto param = [&](const char* name, const std::string& fallback) {
        const char* v = req.url_params.get(name);
        return v ? std::string(v) : fallback;
    };
    std::string spec_id = param("spec", saved ? saved->label_spec : layout::kDefaultLabelSpecId);
    std::string order_name = param("order", saved ? saved->part_order : "heaviest");
    std::string hide = param("hide", saved ? saved->hidden_parts : labels_pdf::LabelOptions().hidden_csv());

    const layout::LabelSpec* spec = layout::find_label_spec(spec_id);
    if (!spec) spec = &layout::default_label_spec();  // a saved stock that's since been dropped
    auto order = ordering::parse_part_order(order_name);
    if (!order) {
        *error = "order must be heaviest, lightest or sheet";
        return std::nullopt;
    }
    // `hide` is the full list of parts switched off.
    auto options = labels_pdf::LabelOptions::from_hidden(hide, error);
    if (!options) return std::nullopt;
    return EffectiveDesign{spec, *order, *options};
}

// Turns a failed Google call into a response the organizer can act on.
crow::response google_error(const std::exception& e) {
    if (auto* big = dynamic_cast<const TooBig*>(&e)) {
        return crow::response(413, std::string("Too big: ") + big->what() + ".");
    }
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

// "Download all": every report for one run, in a single .zip — labels,
// packing checklist, parts list and lot counts (PDF and CSV), plus the
// sheet check as text. Consumes pivot.records.
std::string build_bundle(PivotResult& pivot, const layout::LabelSpec& spec,
                         ordering::PartOrder order, const labels_pdf::LabelOptions& options,
                         const std::string& stem, const std::string& image_cache) {
    const auto sort = reports::SortBy::kLastName;
    auto parts = ordering::summarize_parts(pivot.records, order);
    std::string lots_csv = reports::lot_counts_csv(pivot.records, sort);
    auto lots_pdf = reports::lot_counts_pdf(pivot.records, sort);

    std::string check = std::to_string(pivot.records.size()) + " labels\n";
    if (pivot.issues.empty()) check += "No issues found.\n";
    for (const auto& i : pivot.issues) {
        check += "Row " + std::to_string(i.row) + " (" + i.kind + "): " + i.detail + "\n";
    }

    auto records = ordering::order_records(std::move(pivot.records), order);
    auto as_str = [](const std::vector<uint8_t>& b) {
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    };
    return zip_writer::zip({
        {stem + " labels.pdf", as_str(labels_pdf::build_labels_pdf(records, image_cache, spec, options))},
        {stem + " packing checklist.pdf", as_str(reports::checklist_pdf(records))},
        {stem + " parts.pdf", as_str(reports::parts_pdf(parts))},
        {stem + " parts.csv", reports::parts_csv(parts)},
        {stem + " lot counts.pdf", as_str(lots_pdf)},
        {stem + " lot counts.csv", lots_csv},
        {stem + " sheet check.txt", check},
    });
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

// True if a JSON request body has `key` as a string (anything else would
// throw on .s() and surface as a 500).
bool has_string(const crow::json::rvalue& json, const char* key) {
    return json.has(key) && json[key].t() == crow::json::type::String;
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

    Guards guards(cfg);
    g_guards = &guards;
    g_google_enabled = cfg.google_enabled();
    if (!cfg.google_enabled()) {
        std::cerr << "Google sign-in is off (no GOOGLE_OAUTH_CLIENT_ID): uploads only" << std::endl;
    }
    bricklink::CatalogCache catalog(cfg.data_dir + "/bricklink");
    g_catalog = &catalog;
    catalog.get();  // load (and log) now rather than on the first report
    if (cfg.google_enabled() && guards.allowlist.empty()) {
        std::cerr << "warning: ALLOWED_EMAILS is not set — any Google account that can pass "
                     "the OAuth consent screen can sign in" << std::endl;
    }

    App app;
    app.get_middleware<SecurityMiddleware>().origin = app_origin(cfg);
    app.get_middleware<SecurityMiddleware>().https = cfg.https();
    // Crow's default INFO access log prints the full request path, which
    // for /auth/callback includes the (single-use, but still sensitive)
    // authorization code and session-bound state as a query string. Drop
    // to Warning so that never lands in logs/log aggregators.
    app.loglevel(crow::LogLevel::Warning);
    crow::mustache::set_global_base("templates");

    CROW_ROUTE(app, "/healthz")([]() {
        return crow::response(200, "ok");
    });

    // Which build is running: "1.2.0" for a release, "canary-<sha>" for a
    // build of the head of master, "dev" for a local build. Baked into the
    // image by the Dockerfile's VERSION build arg.
    const std::string version = std::getenv("LUGBULK_VERSION") ? std::getenv("LUGBULK_VERSION") : "dev";
    CROW_ROUTE(app, "/version")([version]() { return crow::response(200, version); });

    CROW_ROUTE(app, "/")([&, version](const crow::request& req) {
        // Signed in: saved Google Sheets plus uploads. Not signed in (or no
        // Google configured): uploads only, plus a sign-in button if Google
        // is available.
        auto user = current_user(*db, req);
        // Mustache HTML-escapes {{email}} automatically, so a display name
        // containing markup can't break out of the page.
        auto tmpl = crow::mustache::load("dashboard.html");
        crow::mustache::context ctx;
        std::string nonce = crypto::random_hex_token(16);
        ctx["nonce"] = nonce;
        ctx["signed_in"] = user.has_value();
        ctx["google"] = cfg.google_enabled();
        ctx["email"] = user ? user->email : std::string();
        ctx["version"] = version;
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
        res.set_header("Content-Security-Policy", csp_for(nonce));
        return res;
    });

    // Step 1: redirect the browser to Google's consent screen. A random
    // `state` value is generated, stashed in a short-lived HttpOnly cookie,
    // and echoed back by Google in the callback — compared there to guard
    // against CSRF (an attacker linking a victim straight into /auth/callback
    // with an authorization code of the attacker's own account).
    CROW_ROUTE(app, "/auth/login")([&cfg](const crow::request&) {
        if (!cfg.google_enabled()) return crow::response(404, "Google sign-in isn't set up on this server.");
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
        if (!cfg.google_enabled()) return crow::response(404, "Google sign-in isn't set up on this server.");
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
            if (!info.email_verified || !g_guards->allowlist.allows(info.email)) {
                // Nothing is stored for a refused account.
                std::cerr << "auth/callback: sign-in refused for a non-allowlisted account"
                          << std::endl;
                return crow::response(403, "This Google account isn't allowed to use this app — "
                                           "ask the organizer to add it.");
            }

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
            db->trim_sessions(user.id, kMaxSessionsPerUser);

            crow::response res(302);
            res.set_header("Location", local_url(cfg, "/"));
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
            res.set_header("Location", local_url(cfg, "/"));
            res.add_header("Set-Cookie",
                            std::string(kSessionCookie) + "=; " + clear_cookie_attrs(cfg));
            // Drop anything the browser kept from this session (on a shared
            // computer, the next person shouldn't find it).
            res.set_header("Clear-Site-Data", "\"cache\"");
            return res;
        });

    // Search/list the user's own Drive for spreadsheets they can pick from
    // (does not touch our `sheets` table — this is live Drive metadata,
    // not what's already saved). `?q=` is an optional name substring.
    CROW_ROUTE(app, "/sheets/search")([&cfg, &db](const crow::request& req) {
        auto user = current_user(*db, req);
        if (!user) return crow::response(401, "not logged in");

        if (auto retry = g_guards->search_per_user.take(std::to_string(user->id))) {
            return too_many(*retry, "Searching too fast — wait a few seconds.");
        }
        std::string query;
        if (auto q = req.url_params.get("q")) query = q;
        if (query.size() > 200) return crow::response(400, "search text too long");

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
        [&cfg, &db](const crow::request& req) {
            auto user = current_user(*db, req);
            if (!user) return crow::response(401, "not logged in");

            auto json = crow::json::load(req.body);
            if (!json || json.t() != crow::json::type::Object || !has_string(json, "sheet_id") ||
                !has_string(json, "display_name")) {
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

            if (db->count_sheets(user->id) >= kMaxSavedSheets) {
                return crow::response(409, "You have " + std::to_string(kMaxSavedSheets) +
                                               " saved sheets — remove one first.");
            }
            if (auto retry = g_guards->search_per_user.take(std::to_string(user->id))) {
                return too_many(*retry, "Adding sheets too fast — wait a few seconds.");
            }
            // Only save sheets the user can actually open. Besides catching
            // typos, this is what lets a sheet's shared label design be
            // edited only by people with access to that sheet.
            try {
                oauth::fetch_spreadsheet_title(mint_access_token(cfg, *db, user->id), sheet_id);
            } catch (const std::exception& e) {
                if (auto* http = dynamic_cast<const oauth::HttpError*>(&e);
                    http && (http->status == 403 || http->status == 404)) {
                    return crow::response(403, "Your Google account can't open that sheet.");
                }
                return google_error(e);
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
        auto ticket = start_job(user->id);
        if (!ticket.ticket) return std::move(ticket.refused);

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

    // Live design preview: one page of built-in sample labels in the given
    // stock and design (spec, order, hide). No sheet is read, so it's cheap
    // enough to re-render on every switch flip.
    CROW_ROUTE(app, "/preview")([&cfg, &db](const crow::request& req) {
        auto user = current_user(*db, req);
        if (auto retry = g_guards->preview_per_user.take(visitor_key(req, user))) {
            return too_many(*retry, "Preview is updating too fast — wait a moment.");
        }
        std::string error;
        auto design = resolve_design(std::nullopt, req, &error);
        if (!design) return crow::response(400, error);
        auto records = samples::sample_records();
        // Roll stock is one label per page: show a few.
        size_t pages = design->spec->per_sheet() == 1 ? 3 : 1;
        auto pdf = labels_pdf::build_labels_pdf(records, cfg.data_dir + "/image_cache", *design->spec,
                                                design->options, pages);
        crow::response res(200, as_string(pdf));
        res.set_header("Content-Type", "application/pdf");
        return res;
    });

    // Printer alignment test page for a label stock.
    CROW_ROUTE(app, "/test-page")([&db](const crow::request& req) {
        auto user = current_user(*db, req);
        if (auto retry = g_guards->preview_per_user.take(visitor_key(req, user))) {
            return too_many(*retry, "Too fast — wait a moment.");
        }
        const char* spec_id = req.url_params.get("spec");
        const layout::LabelSpec* spec = spec_id ? layout::find_label_spec(spec_id)
                                                : &layout::default_label_spec();
        if (!spec) return crow::response(400, "unknown label stock");
        return attachment("application/pdf", spec->id + " alignment test.pdf",
                          as_string(labels_pdf::build_test_page(*spec)));
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
        auto ticket = start_job(user->id);
        if (!ticket.ticket) return std::move(ticket.refused);

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
            auto [res, item_count] = render(pivot, *owned);
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


    // Labels in the sheet's saved design; spec/order/hide query parameters
    // override it for this download.
    CROW_ROUTE(app, "/sheets/<int>/labels").methods(crow::HTTPMethod::Post)(
        [&](const crow::request& req, int64_t row_id) {
            return generate(req, row_id, "labels", [&](PivotResult& pivot, const SheetOwnership& sheet) {
                std::string error;
                auto design = resolve_design(db->get_design(sheet.sheet_id), req, &error);
                if (!design) return std::make_pair(crow::response(400, error), 0);
                auto records = ordering::order_records(std::move(pivot.records), design->order);
                std::vector<uint8_t> pdf = labels_pdf::build_labels_pdf(
                    records, cfg.data_dir + "/image_cache", *design->spec, design->options);
                return std::make_pair(
                    attachment("application/pdf", safe_filename_stem(sheet.display_name) + " labels.pdf",
                               as_string(pdf)),
                    static_cast<int>(records.size()));
            });
        });

    // Packing checklist: one page per person, in label order.
    // Everything for the sheet in one .zip, in its saved design.
    CROW_ROUTE(app, "/sheets/<int>/all").methods(crow::HTTPMethod::Post)(
        [&](const crow::request& req, int64_t row_id) {
            return generate(req, row_id, "bundle", [&](PivotResult& pivot, const SheetOwnership& sheet) {
                std::string error;
                auto design = resolve_design(db->get_design(sheet.sheet_id), req, &error);
                if (!design) return std::make_pair(crow::response(400, error), 0);
                int count = static_cast<int>(pivot.records.size());
                std::string stem = safe_filename_stem(sheet.display_name);
                return std::make_pair(
                    attachment("application/zip", stem + " labels and reports.zip",
                               build_bundle(pivot, *design->spec, design->order, design->options, stem,
                                            cfg.data_dir + "/image_cache")),
                    count);
            });
        });

    CROW_ROUTE(app, "/sheets/<int>/checklist").methods(crow::HTTPMethod::Post)(
        [&](const crow::request& req, int64_t row_id) {
            return generate(req, row_id, "checklist", [&](PivotResult& pivot, const SheetOwnership& sheet) {
                std::string error;
                auto design = resolve_design(db->get_design(sheet.sheet_id), req, &error);
                if (!design) return std::make_pair(crow::response(400, error), 0);
                auto records = ordering::order_records(std::move(pivot.records), design->order);
                return std::make_pair(
                    attachment("application/pdf",
                               safe_filename_stem(sheet.display_name) + " packing checklist.pdf",
                               as_string(reports::checklist_pdf(records))),
                    static_cast<int>(records.size()));
            });
        });

    // The sheet's label design (shared by everyone who has the sheet saved).
    CROW_ROUTE(app, "/sheets/<int>/design")([&db](const crow::request& req, int64_t row_id) {
        auto user = current_user(*db, req);
        if (!user) return crow::response(401, "not logged in");
        auto owned = db->find_owned_sheet(user->id, row_id);
        if (!owned) return crow::response(404, "sheet not found");
        std::string error;
        auto design = resolve_design(db->get_design(owned->sheet_id), crow::request(), &error);
        crow::json::wvalue body;
        body["spec"] = design->spec->id;
        body["order"] = std::string(design->order == ordering::PartOrder::kHeaviest   ? "heaviest"
                                    : design->order == ordering::PartOrder::kLightest ? "lightest"
                                                                                      : "sheet");
        body["hide"] = design->options.hidden_csv();
        crow::response res(200, body.dump());
        res.set_header("Content-Type", "application/json");
        return res;
    });

    CROW_ROUTE(app, "/sheets/<int>/design").methods(crow::HTTPMethod::Put)(
        [&cfg, &db](const crow::request& req, int64_t row_id) {
            auto user = current_user(*db, req);
            if (!user) return crow::response(401, "not logged in");
            auto owned = db->find_owned_sheet(user->id, row_id);
            if (!owned) return crow::response(404, "sheet not found");
            if (auto retry = g_guards->preview_per_user.take(std::to_string(user->id))) {
                return too_many(*retry, "Saving too fast — wait a moment.");
            }
            // The design is shared by everyone with this Google Sheet, so only
            // someone who can actually open it may change it. Sheets saved
            // before that was checked on save are checked once here.
            if (!owned->verified) {
                try {
                    oauth::fetch_spreadsheet_title(mint_access_token(cfg, *db, user->id),
                                                   owned->sheet_id);
                    db->mark_sheet_verified(owned->sheet_row_id);
                } catch (const std::exception& e) {
                    if (auto* http = dynamic_cast<const oauth::HttpError*>(&e);
                        http && (http->status == 403 || http->status == 404)) {
                        return crow::response(403, "Your Google account can't open this sheet.");
                    }
                    return google_error(e);
                }
            }
            auto json = crow::json::load(req.body);
            if (!json || json.t() != crow::json::type::Object || !has_string(json, "spec") ||
                !has_string(json, "order") || !has_string(json, "hide")) {
                return crow::response(400, R"(expected {"spec":..., "order":..., "hide":"..."})");
            }
            const layout::LabelSpec* spec = layout::find_label_spec(std::string(json["spec"].s()));
            std::string order = json["order"].s();
            std::string hide = json["hide"].s();
            std::string error;
            if (!spec) return crow::response(400, "unknown label stock");
            if (!ordering::parse_part_order(order)) return crow::response(400, "bad part order");
            auto opts = labels_pdf::LabelOptions::from_hidden(hide, &error);
            if (!opts) return crow::response(400, error);
            db->put_design(owned->sheet_id, Design{spec->id, order, opts->hidden_csv()}, user->id);
            return crow::response(200, "saved");
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
                            [&](PivotResult& pivot, const SheetOwnership& sheet) {
                const std::string& name = sheet.display_name;
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
            if (!format) return crow::response(400, "format must be 'csv' or 'pdf'");

            return generate(req, row_id, "parts", [&](PivotResult& pivot, const SheetOwnership& sheet) {
                const std::string& name = sheet.display_name;
                std::string error;
                auto design = resolve_design(db->get_design(sheet.sheet_id), req, &error);
                if (!design) return std::make_pair(crow::response(400, error), 0);
                auto parts = ordering::summarize_parts(pivot.records, design->order);
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

    // --- Uploads: anyone, signed in or not ---------------------------------
    // The request body is the .xlsx or .csv file itself; the design comes
    // from the query string (spec/order/hide) and `X-File-Name` (optional)
    // names the download. The file is read in memory and never stored.
    auto upload = [&cfg](const crow::request& req, auto render) {
        auto admission = start_anonymous_job(client_ip(req));
        if (!admission.ticket) return std::move(admission.refused);
        if (req.body.empty()) return crow::response(400, "Choose an .xlsx or .csv file first.");
        std::string error;
        auto design = resolve_design(std::nullopt, req, &error);
        if (!design) return crow::response(400, error);
        // Download names follow the uploaded file's, minus its extension.
        std::string name = req.get_header_value("X-File-Name");
        if (size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);
        std::string stem = safe_filename_stem(name);
        if (stem == "sheet") stem = "order sheet";
        try {
            PivotResult pivot = pivot_upload(req.body);
            if (pivot.records.empty() && !render.allows_empty) {
                return crow::response(422, std::string("No orders found in that file — it needs the '") +
                                               layout::kSourceTab +
                                               "' tab's columns (Element ID / Part Number and a "
                                               "column per person). Try Check file to see why.");
            }
            return render.fn(pivot, *design, stem);
        } catch (const spreadsheet::Error& e) {
            return crow::response(400, e.what());
        } catch (const TooBig& e) {
            return crow::response(413, std::string("Too big: ") + e.what() + ".");
        } catch (const std::exception& e) {
            std::cerr << "upload failed: " << e.what() << std::endl;
            return crow::response(500, "Couldn't read that file — try again.");
        }
    };
    struct Render {
        bool allows_empty;
        std::function<crow::response(PivotResult&, const EffectiveDesign&, const std::string&)> fn;
    };
    const std::string image_cache = cfg.data_dir + "/image_cache";

    CROW_ROUTE(app, "/upload/all").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        return upload(req, Render{false, [&](PivotResult& pivot, const EffectiveDesign& d,
                                             const std::string& stem) {
            return attachment("application/zip", stem + " labels and reports.zip",
                              build_bundle(pivot, *d.spec, d.order, d.options, stem, image_cache));
        }});
    });
    CROW_ROUTE(app, "/upload/check").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        return upload(req, Render{true, [](PivotResult& pivot, const EffectiveDesign&, const std::string&) {
            crow::response res(200, sheet_error_json(pivot));
            res.set_header("Content-Type", "application/json");
            return res;
        }});
    });
    CROW_ROUTE(app, "/upload/labels").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        return upload(req, Render{false, [&](PivotResult& pivot, const EffectiveDesign& d,
                                             const std::string& stem) {
            auto records = ordering::order_records(std::move(pivot.records), d.order);
            return attachment("application/pdf", stem + " labels.pdf",
                              as_string(labels_pdf::build_labels_pdf(records, image_cache, *d.spec,
                                                                     d.options)));
        }});
    });
    CROW_ROUTE(app, "/upload/checklist").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        return upload(req, Render{false, [](PivotResult& pivot, const EffectiveDesign& d,
                                            const std::string& stem) {
            auto records = ordering::order_records(std::move(pivot.records), d.order);
            return attachment("application/pdf", stem + " packing checklist.pdf",
                              as_string(reports::checklist_pdf(records)));
        }});
    });
    CROW_ROUTE(app, "/upload/parts").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        bool csv = req.url_params.get("format") && std::string(req.url_params.get("format")) == "csv";
        return upload(req, Render{false, [csv](PivotResult& pivot, const EffectiveDesign& d,
                                               const std::string& stem) {
            auto parts = ordering::summarize_parts(pivot.records, d.order);
            return csv ? attachment("text/csv; charset=utf-8", stem + " parts.csv", reports::parts_csv(parts))
                       : attachment("application/pdf", stem + " parts.pdf",
                                    as_string(reports::parts_pdf(parts)));
        }});
    });
    CROW_ROUTE(app, "/upload/lots").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
        bool csv = req.url_params.get("format") && std::string(req.url_params.get("format")) == "csv";
        return upload(req, Render{false, [csv](PivotResult& pivot, const EffectiveDesign&,
                                               const std::string& stem) {
            const auto sort = reports::SortBy::kLastName;
            return csv ? attachment("text/csv; charset=utf-8", stem + " lot counts.csv",
                                    reports::lot_counts_csv(pivot.records, sort))
                       : attachment("application/pdf", stem + " lot counts.pdf",
                                    as_string(reports::lot_counts_pdf(pivot.records, sort)));
        }});
    });

    app.port(8080).multithreaded().run();
}
