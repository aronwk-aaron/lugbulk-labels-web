#include "config.h"

#include <cstdlib>

namespace lugbulk {

namespace {

std::string require_env(const char* name) {
    const char* v = std::getenv(name);
    if (!v || *v == '\0') {
        throw std::runtime_error(std::string("missing required env var: ") + name);
    }
    return std::string(v);
}

std::string optional_env(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v != '\0') ? std::string(v) : fallback;
}

int positive_int_env(const char* name, int fallback) {
    std::string v = optional_env(name, "");
    if (v.empty()) return fallback;
    try {
        int n = std::stoi(v);
        if (n > 0) return n;
    } catch (const std::exception&) {
    }
    throw std::runtime_error(std::string("env var ") + name + " must be a positive integer");
}

}  // namespace

Config Config::load_from_env() {
    Config cfg;
    cfg.google_client_id = require_env("GOOGLE_OAUTH_CLIENT_ID");
    cfg.google_client_secret = require_env("GOOGLE_OAUTH_CLIENT_SECRET");
    cfg.google_redirect_uri =
        optional_env("GOOGLE_OAUTH_REDIRECT_URI", "http://localhost:8080/auth/callback");
    cfg.token_encryption_key_b64 = require_env("TOKEN_ENCRYPTION_KEY");
    cfg.data_dir = optional_env("LUGBULK_DATA_DIR", ".");
    cfg.allowed_emails = optional_env("ALLOWED_EMAILS", "");
    cfg.trust_proxy = optional_env("TRUST_PROXY", "") == "1";
    cfg.max_concurrent_jobs = positive_int_env("MAX_CONCURRENT_JOBS", 2);
    cfg.bricklink_daily_calls = positive_int_env("BRICKLINK_DAILY_CALLS", 4000);
    cfg.bricklink.consumer_key = optional_env("BRICKLINK_CONSUMER_KEY", "");
    cfg.bricklink.consumer_secret = optional_env("BRICKLINK_CONSUMER_SECRET", "");
    cfg.bricklink.token = optional_env("BRICKLINK_TOKEN", "");
    cfg.bricklink.token_secret = optional_env("BRICKLINK_TOKEN_SECRET", "");
    return cfg;
}

}  // namespace lugbulk
