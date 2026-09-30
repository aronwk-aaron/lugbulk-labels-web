#include "oauth.h"

#include <curl/curl.h>

#include <memory>
#include <stdexcept>

#include "crow/json.h"

namespace lugbulk::oauth {

namespace {

constexpr const char* kAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char* kTokenEndpoint = "https://oauth2.googleapis.com/token";
constexpr const char* kUserinfoEndpoint = "https://openidconnect.googleapis.com/v1/userinfo";
constexpr const char* kSheetsValuesEndpoint = "https://sheets.googleapis.com/v4/spreadsheets/";
// drive.file is Google's non-sensitive per-file scope: it covers only the
// files the user opens with this app through the Google Picker, and
// nothing else in their Drive. The Sheets API (values.get) works on those
// files under it, so no restricted or sensitive scope is needed.
constexpr const char* kScopes = "openid email https://www.googleapis.com/auth/drive.file";

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};
// Initialized once at process start (constructed on first use of this TU).
const CurlGlobal g_curl_global;

// Largest Google API response we'll buffer. A capped "Order Here" range
// is a few MB at most; anything bigger aborts the transfer rather than
// growing without bound.
constexpr size_t kMaxResponseBytes = 32 * 1024 * 1024;

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    if (out->size() + size * nmemb > kMaxResponseBytes) return 0;  // curl aborts: CURLE_WRITE_ERROR
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string url_encode(CURL* curl, const std::string& s) {
    char* enc = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
    std::string result = enc ? enc : "";
    if (enc) curl_free(enc);
    return result;
}

// POSTs `form` (application/x-www-form-urlencoded body already assembled
// by the caller) to `url` and returns the response body. Throws on
// transport failure or non-2xx status. Never includes request body
// (contains the client secret / auth code) in the exception message.
std::string http_post_form(const std::string& url, const std::string& body) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("oauth error: curl init failed");

    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    // Enforce TLS verification explicitly (defaults are on, but this is
    // security-sensitive enough to be explicit rather than rely on defaults).
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl.get());
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("oauth error: request failed: ") +
                                  curl_easy_strerror(rc));
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        throw HttpError("oauth error: token endpoint returned HTTP " + std::to_string(status),
                        status);
    }
    return response;
}

std::string http_get_bearer(const std::string& url, const std::string& bearer_token) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("oauth error: curl init failed");

    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + bearer_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl.get());
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("oauth error: request failed: ") +
                                  curl_easy_strerror(rc));
    }
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        throw HttpError("oauth error: Google API returned HTTP " + std::to_string(status),
                        status);
    }
    return response;
}

}  // namespace

bool has_scope(const std::string& scopes, const std::string& wanted) {
    size_t pos = 0;
    while (pos <= scopes.size()) {
        size_t end = scopes.find(' ', pos);
        if (end == std::string::npos) end = scopes.size();
        if (end - pos == wanted.size() && scopes.compare(pos, end - pos, wanted) == 0) return true;
        pos = end + 1;
    }
    return false;
}

std::string build_authorize_url(const Config& cfg, const std::string& state,
                                 bool force_consent) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("oauth error: curl init failed");

    std::string url = std::string(kAuthEndpoint) + "?" +
                       "client_id=" + url_encode(curl.get(), cfg.google_client_id) +
                       "&redirect_uri=" + url_encode(curl.get(), cfg.google_redirect_uri) +
                       "&response_type=code" +
                       "&scope=" + url_encode(curl.get(), kScopes) +
                       "&access_type=offline" +
                       "&state=" + url_encode(curl.get(), state);
    // No include_granted_scopes: someone who signed in back when the app
    // asked for spreadsheets.readonly + drive.metadata.readonly gets a token
    // for drive.file alone, not one that carries the old scopes forward.
    if (force_consent) {
        url += "&prompt=consent";
    }
    return url;
}

TokenResponse exchange_code(const Config& cfg, const std::string& code) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("oauth error: curl init failed");

    std::string body = "code=" + url_encode(curl.get(), code) +
                        "&client_id=" + url_encode(curl.get(), cfg.google_client_id) +
                        "&client_secret=" + url_encode(curl.get(), cfg.google_client_secret) +
                        "&redirect_uri=" + url_encode(curl.get(), cfg.google_redirect_uri) +
                        "&grant_type=authorization_code";

    std::string response = http_post_form(kTokenEndpoint, body);

    auto json = crow::json::load(response);
    if (!json || !json.has("access_token")) {
        throw std::runtime_error("oauth error: malformed token response");
    }
    TokenResponse tr;
    tr.access_token = json["access_token"].s();
    if (json.has("refresh_token")) tr.refresh_token = json["refresh_token"].s();
    if (json.has("expires_in")) tr.expires_in = json["expires_in"].i();
    return tr;
}

TokenResponse refresh_access_token(const Config& cfg, const std::string& refresh_token) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("oauth error: curl init failed");

    std::string body = "refresh_token=" + url_encode(curl.get(), refresh_token) +
                        "&client_id=" + url_encode(curl.get(), cfg.google_client_id) +
                        "&client_secret=" + url_encode(curl.get(), cfg.google_client_secret) +
                        "&grant_type=refresh_token";

    std::string response = http_post_form(kTokenEndpoint, body);

    auto json = crow::json::load(response);
    if (!json || !json.has("access_token")) {
        throw std::runtime_error("oauth error: malformed refresh response");
    }
    TokenResponse tr;
    tr.access_token = json["access_token"].s();
    if (json.has("expires_in")) tr.expires_in = json["expires_in"].i();
    if (json.has("scope")) tr.scope = json["scope"].s();
    // Google normally does not re-issue a refresh_token on a refresh grant;
    // leave tr.refresh_token empty (caller keeps the one already stored).
    return tr;
}

UserInfo fetch_userinfo(const std::string& access_token) {
    std::string response = http_get_bearer(kUserinfoEndpoint, access_token);

    auto json = crow::json::load(response);
    if (!json || !json.has("sub") || !json.has("email")) {
        throw std::runtime_error("oauth error: malformed userinfo response");
    }
    UserInfo info;
    info.sub = json["sub"].s();
    info.email = json["email"].s();
    info.email_verified = json.has("email_verified") && json["email_verified"].t() == crow::json::type::True;
    return info;
}

std::string fetch_spreadsheet_title(const std::string& access_token,
                                    const std::string& spreadsheet_id) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("oauth error: curl init failed");
    std::string url = std::string(kSheetsValuesEndpoint) + url_encode(curl.get(), spreadsheet_id) +
                      "?fields=" + url_encode(curl.get(), "properties.title");
    auto json = crow::json::load(http_get_bearer(url, access_token));
    if (!json || !json.has("properties") || !json["properties"].has("title")) {
        throw std::runtime_error("oauth error: malformed spreadsheet response");
    }
    return std::string(json["properties"]["title"].s());
}

std::vector<std::vector<std::string>> fetch_sheet_values(const std::string& access_token,
                                                           const std::string& spreadsheet_id,
                                                           const std::string& range) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("oauth error: curl init failed");

    std::string url = std::string(kSheetsValuesEndpoint) +
                       url_encode(curl.get(), spreadsheet_id) + "/values/" +
                       url_encode(curl.get(), range) +
                       "?valueRenderOption=FORMATTED_VALUE";

    std::string response = http_get_bearer(url, access_token);

    auto json = crow::json::load(response);
    if (!json) {
        throw std::runtime_error("oauth error: malformed Sheets values response");
    }

    std::vector<std::vector<std::string>> rows;
    if (!json.has("values")) return rows;  // empty range comes back with no "values" key at all

    auto values = json["values"];
    rows.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        auto row = values[i];
        std::vector<std::string> out_row;
        out_row.reserve(row.size());
        for (size_t j = 0; j < row.size(); ++j) {
            // Cells are normally strings; Sheets can return a bare number
            // for e.g. numeric qty cells depending on formatting, so accept
            // either rather than throwing on a non-string cell — rvalue's
            // string conversion operator already handles both.
            out_row.push_back(static_cast<std::string>(row[j]));
        }
        rows.push_back(std::move(out_row));
    }
    return rows;
}

}  // namespace lugbulk::oauth
