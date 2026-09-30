// Google OAuth 2.0 authorization-code flow (with offline access, so we get
// a refresh token) + fetching the logged-in user's stable id/email.
//
// Only basic profile + drive.file are requested: drive.file reaches just the
// files the user picks for this app with the Google Picker.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"

namespace lugbulk::oauth {

// A Google API call that got a non-2xx answer. `status` lets callers turn
// e.g. a 403/404 on a sheet into a useful message for the user.
struct HttpError : std::runtime_error {
    long status;
    HttpError(const std::string& what, long status_code)
        : std::runtime_error(what), status(status_code) {}
};

// Everything Google gave us for one authorization-code exchange.
struct TokenResponse {
    std::string access_token;
    std::string refresh_token;  // empty if Google didn't issue a new one
    int expires_in = 0;
    std::string scope;  // space-separated scopes granted (refresh grant only)
};

// The per-file Drive scope the app signs in with.
inline constexpr const char* kDriveFileScope = "https://www.googleapis.com/auth/drive.file";

// Whether a space-separated OAuth `scope` string includes `wanted`.
bool has_scope(const std::string& scopes, const std::string& wanted);

struct UserInfo {
    bool email_verified = false;
    std::string sub;    // stable Google account id
    std::string email;
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

// Fetches the raw cell values of `range` (e.g. "'Order Here'!A1:CT") from a
// spreadsheet via the Sheets API, read-only (spreadsheets.values.get —
// never writes/updates/appends). Returns rows as given by Google: ragged
// (a row's length is only as long as its last non-empty cell), values are
// always strings (Sheets API's default UNFORMATTED_VALUE is not used, so
// numbers come back display-formatted, e.g. "2,000" — callers must strip
// thousands separators before parsing). Throws std::runtime_error on any
// transport/HTTP failure or malformed response.
// The spreadsheet's title — used to confirm the user can actually open a
// sheet before saving it. Throws HttpError (403/404) if they can't.
std::string fetch_spreadsheet_title(const std::string& access_token,
                                    const std::string& spreadsheet_id);

std::vector<std::vector<std::string>> fetch_sheet_values(const std::string& access_token,
                                                           const std::string& spreadsheet_id,
                                                           const std::string& range);

}  // namespace lugbulk::oauth
