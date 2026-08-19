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

}  // namespace

Config Config::load_from_env() {
    Config cfg;
    cfg.google_client_id = require_env("GOOGLE_OAUTH_CLIENT_ID");
    cfg.google_client_secret = require_env("GOOGLE_OAUTH_CLIENT_SECRET");
    cfg.google_redirect_uri =
        optional_env("GOOGLE_OAUTH_REDIRECT_URI", "http://localhost:8080/auth/callback");
    cfg.token_encryption_key_b64 = require_env("TOKEN_ENCRYPTION_KEY");
    cfg.data_dir = optional_env("LUGBULK_DATA_DIR", ".");
    return cfg;
}

}  // namespace lugbulk
