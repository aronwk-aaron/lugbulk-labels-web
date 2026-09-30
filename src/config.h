// Environment-derived configuration. Loaded once at startup; nothing here
// is ever logged verbatim (client secret / token key are secrets).
#pragma once

#include <stdexcept>
#include <string>

#include "bricklink.h"

namespace lugbulk {

struct Config {
    std::string google_client_id;
    std::string google_client_secret;
    std::string google_redirect_uri;
    std::string token_encryption_key_b64;  // 32 raw bytes, base64-encoded
    std::string data_dir;                  // holds sqlite db; defaults to "."
    // Optional BrickLink API credentials (BRICKLINK_CONSUMER_KEY /
    // _CONSUMER_SECRET / _TOKEN / _TOKEN_SECRET): part weights for label
    // order, and colors a sheet is missing. Unset = estimate weights.
    bricklink::Credentials bricklink;
    // ALLOWED_EMAILS: comma-separated addresses and/or "@domain"s allowed to
    // sign in. Empty = any Google account (see limits::Allowlist).
    std::string allowed_emails;
    // TRUST_PROXY=1: take the client IP from X-Forwarded-For (only behind a
    // reverse proxy that sets it; otherwise clients could spoof it).
    bool trust_proxy = false;
    // MAX_CONCURRENT_JOBS: report generations running at once, server-wide.
    int max_concurrent_jobs = 2;
    // BRICKLINK_DAILY_CALLS: BrickLink API calls allowed per UTC day
    // (BrickLink's own limit is 5,000).
    int bricklink_daily_calls = 4000;

    // Reads required env vars, throws std::runtime_error naming the missing
    // key (never the value) if something required is absent.
    static Config load_from_env();
};

}  // namespace lugbulk
