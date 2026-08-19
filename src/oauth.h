// Google OAuth 2.0 authorization-code flow (with offline access, so we get
// a refresh token) + fetching the logged-in user's stable id/email.
//
// Only the Sheets read-only scope + basic profile are requested — no scope
// grants write access or anything beyond what the app needs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace lugbulk::oauth {

// Everything Google gave us for one authorization-code exchange.
struct TokenResponse {
    std::string access_token;
    std::string refresh_token;  // empty if Google didn't issue a new one
    int expires_in = 0;
};

struct UserInfo {
    std::string sub;    // stable Google account id
    std::string email;
};

struct SheetFile {
    std::string id;             // Drive file id == Sheets spreadsheet id
    std::string name;
    std::string modified_time;  // RFC3339, as returned by Drive
};

// Builds the URL to redirect the browser to. `state` must be a
// caller-generated random value, stored server-side (session-bound) and
// checked again in the callback to prevent CSRF. `force_consent` requests
// `prompt=consent` so Google reliably re-issues a refresh_token even for a
// user who authorized before (needed the first time we store one; Google
// normally only sends it on the very first consent).
std::string build_authorize_url(const Config& cfg, const std::string& state,
                                 bool force_consent);

// Exchanges an authorization `code` for tokens. Throws std::runtime_error
// on any transport or non-2xx response; the message never includes the
// client secret or token values, only the HTTP status / a fixed reason.
TokenResponse exchange_code(const Config& cfg, const std::string& code);

// Uses `refresh_token` to mint a fresh access token (short-lived, used
// only in-process for the immediate Sheets API call; never stored).
TokenResponse refresh_access_token(const Config& cfg, const std::string& refresh_token);

// Calls Google's userinfo endpoint with a valid access token.
UserInfo fetch_userinfo(const std::string& access_token);

// Lists Google Sheets files the user can access, via the Drive API,
// restricted to non-trashed spreadsheets and (via drive.metadata.readonly)
// metadata only — file contents are never fetched through this call.
// `query` is an optional case-insensitive substring filter on file name;
// empty returns the user's most-recently-modified spreadsheets. Results
// are capped at `limit` (Drive API pageSize; no pagination — a "search and
// pick" UI doesn't need the user's entire Drive enumerated).
std::vector<SheetFile> list_spreadsheets(const std::string& access_token,
                                          const std::string& query, int limit = 25);

// Fetches the raw cell values of `range` (e.g. "'Order Here'!A1:CT") from a
// spreadsheet via the Sheets API, read-only (spreadsheets.values.get —
// never writes/updates/appends). Returns rows as given by Google: ragged
// (a row's length is only as long as its last non-empty cell), values are
// always strings (Sheets API's default UNFORMATTED_VALUE is not used, so
// numbers come back display-formatted, e.g. "2,000" — callers must strip
// thousands separators before parsing). Throws std::runtime_error on any
// transport/HTTP failure or malformed response.
std::vector<std::vector<std::string>> fetch_sheet_values(const std::string& access_token,
                                                           const std::string& spreadsheet_id,
                                                           const std::string& range);

}  // namespace lugbulk::oauth
