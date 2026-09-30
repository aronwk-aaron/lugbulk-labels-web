// Environment-derived configuration. Loaded once at startup; nothing here
// is ever logged verbatim (client secret / token key are secrets).
#pragma once

#include <stdexcept>
#include <string>


namespace lugbulk {

struct Config {
    std::string google_client_id;
    std::string google_client_secret;
    std::string google_redirect_uri;
    std::string token_encryption_key_b64;  // 32 raw bytes, base64-encoded
    std::string data_dir;                  // holds sqlite db; defaults to "."
    // Google Picker (choosing a sheet from Drive in the browser):
    // GOOGLE_API_KEY is a browser API key restricted to the app's origin and
    // the Picker API; GOOGLE_APP_ID is the Cloud project number. Both are
    // handed to the signed-in user's page, so neither is a secret, but they
    // are still never logged.
    std::string google_api_key;
    std::string google_app_id;

    // Google sign-in (saved sheets read live from Google) is on when the
    // OAuth client is configured; uploads work either way.
    bool google_enabled() const { return !google_client_id.empty(); }
    // Picking a new sheet needs the Picker's key and app id as well.
    bool picker_enabled() const {
        return google_enabled() && !google_api_key.empty() && !google_app_id.empty();
    }

    // The app's public URL, e.g. https://lugbulk.example.org (PUBLIC_URL;
    // defaults to the origin of GOOGLE_OAUTH_REDIRECT_URI when Google is
    // on). Used for redirects, the same-origin check, and https-only
    // cookies/HSTS. Empty: redirects stay relative and the same-origin
    // check compares against the request's Host.
    std::string public_url;
    bool https() const { return public_url.rfind("https://", 0) == 0; }
    // ALLOWED_EMAILS: comma-separated addresses and/or "@domain"s allowed to
    // sign in. Empty = any Google account (see limits::Allowlist).
    std::string allowed_emails;
    // TRUST_PROXY=1: take the client IP from X-Forwarded-For (only behind a
    // reverse proxy that sets it; otherwise clients could spoof it).
    bool trust_proxy = false;
    // MAX_CONCURRENT_JOBS: report generations running at once, server-wide.
    int max_concurrent_jobs = 2;

    // Reads required env vars, throws std::runtime_error naming the missing
    // key (never the value) if something required is absent.
    static Config load_from_env();
};

}  // namespace lugbulk
