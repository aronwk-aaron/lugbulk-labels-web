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
    // Google sign-in is optional: without it the app still takes uploaded
    // spreadsheets. Set the client id and secret together, or neither.
    cfg.google_client_id = optional_env("GOOGLE_OAUTH_CLIENT_ID", "");
    cfg.google_client_secret = optional_env("GOOGLE_OAUTH_CLIENT_SECRET", "");
    if (cfg.google_client_id.empty() != cfg.google_client_secret.empty()) {
        throw std::runtime_error(
            "set both GOOGLE_OAUTH_CLIENT_ID and GOOGLE_OAUTH_CLIENT_SECRET, or neither");
    }
    cfg.google_redirect_uri =
        optional_env("GOOGLE_OAUTH_REDIRECT_URI", "http://localhost:8080/auth/callback");
    cfg.token_encryption_key_b64 = cfg.google_enabled() ? require_env("TOKEN_ENCRYPTION_KEY")
                                                        : optional_env("TOKEN_ENCRYPTION_KEY", "");
    cfg.data_dir = optional_env("LUGBULK_DATA_DIR", ".");
    cfg.public_url = optional_env("PUBLIC_URL", "");
    if (cfg.public_url.empty() && cfg.google_enabled()) {
        // scheme://host[:port] of the OAuth redirect URI.
        const std::string& uri = cfg.google_redirect_uri;
        size_t scheme_end = uri.find("://");
        if (scheme_end != std::string::npos) cfg.public_url = uri.substr(0, uri.find('/', scheme_end + 3));
    }
    while (!cfg.public_url.empty() && cfg.public_url.back() == '/') cfg.public_url.pop_back();
    cfg.allowed_emails = optional_env("ALLOWED_EMAILS", "");
    cfg.trust_proxy = optional_env("TRUST_PROXY", "") == "1";
    cfg.max_concurrent_jobs = positive_int_env("MAX_CONCURRENT_JOBS", 2);
    return cfg;
}

}  // namespace lugbulk
