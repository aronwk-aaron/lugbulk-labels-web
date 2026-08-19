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

    // Reads required env vars, throws std::runtime_error naming the missing
    // key (never the value) if something required is absent.
    static Config load_from_env();
};

}  // namespace lugbulk
