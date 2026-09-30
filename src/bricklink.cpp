#include "bricklink.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <vector>

#include "crow/json.h"
#include "crypto.h"

namespace lugbulk::bricklink {

namespace {

constexpr const char* kApiBase = "https://api.bricklink.com/api/store/v1";

// RFC 3986 percent-encoding: everything but unreserved characters.
std::string pct(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// GETs an API path. Returns the response's `data`, or a null value if
// BrickLink says the item doesn't exist.
crow::json::rvalue get(const std::string& path, const Credentials& creds) {
    std::string url = kApiBase + path;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) throw std::runtime_error("bricklink: curl init failed");

    std::string body;
    std::string auth = "Authorization: " + oauth_header("GET", url, creds);
    struct curl_slist* headers = curl_slist_append(nullptr, auth.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode rc = curl_easy_perform(curl.get());
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("bricklink: request failed: ") + curl_easy_strerror(rc));
    }

    auto json = crow::json::load(body);
    if (!json || !json.has("meta")) throw std::runtime_error("bricklink: malformed response");
    int code = json["meta"].has("code") ? static_cast<int>(json["meta"]["code"].i()) : 0;
    if (code == 200) return json.has("data") ? json["data"] : crow::json::rvalue();
    if (code == 400 || code == 404) return crow::json::rvalue();  // unknown element / item
    auto text = [&](const char* key) {
        return json["meta"].has(key) ? std::string(json["meta"][key].s()) : std::string();
    };
    throw ApiError("bricklink: " + text("message") + ": " + text("description") + " (code " +
                   std::to_string(code) + ")");
}

std::optional<double> parse_weight(const crow::json::rvalue& v) {
    double w = 0;
    try {
        w = v.t() == crow::json::type::String ? std::stod(std::string(v.s())) : v.d();
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (w > 0) return w;
    return std::nullopt;  // the catalog uses 0 for "unknown"
}

}  // namespace

std::string oauth_header(const std::string& method, const std::string& url,
                         const Credentials& creds, const std::map<std::string, std::string>& params,
                         std::string nonce, std::string timestamp) {
    if (nonce.empty()) nonce = crypto::random_hex_token(16);
    if (timestamp.empty()) timestamp = std::to_string(std::time(nullptr));
    std::map<std::string, std::string> oauth{
        {"oauth_consumer_key", creds.consumer_key},
        {"oauth_nonce", nonce},
        {"oauth_signature_method", "HMAC-SHA1"},
        {"oauth_timestamp", timestamp},
        {"oauth_token", creds.token},
        {"oauth_version", "1.0"},
    };

    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto& [k, v] : params) pairs.emplace_back(pct(k), pct(v));
    for (const auto& [k, v] : oauth) pairs.emplace_back(pct(k), pct(v));
    std::sort(pairs.begin(), pairs.end());
    std::string param_str;
    for (const auto& [k, v] : pairs) param_str += (param_str.empty() ? "" : "&") + k + "=" + v;

    std::string base = method + "&" + pct(url) + "&" + pct(param_str);
    std::string key = pct(creds.consumer_secret) + "&" + pct(creds.token_secret);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(base.data()), base.size(), digest, &len);
    oauth["oauth_signature"] = crypto::base64_encode(std::vector<uint8_t>(digest, digest + len));

    std::string header = "OAuth ";
    bool first = true;
    for (const auto& [k, v] : oauth) {
        header += (first ? "" : ", ") + pct(k) + "=\"" + pct(v) + "\"";
        first = false;
    }
    return header;
}

PartInfo fetch(const std::string& element_id, const Credentials& creds) {
    PartInfo info;
    auto mappings = get("/item_mapping/" + pct(element_id), creds);
    if (mappings.t() != crow::json::type::List) return info;
    for (size_t i = 0; i < mappings.size(); ++i) {
        const auto& m = mappings[i];
        if (!m.has("item") || !m["item"].has("type") ||
            std::string(m["item"]["type"].s()) != "PART") {
            continue;
        }
        info.part_no = std::string(m["item"]["no"].s());
        if (m.has("color_name")) info.color = std::string(m["color_name"].s());
        break;
    }
    if (info.part_no.empty()) return info;

    auto item = get("/items/PART/" + pct(info.part_no), creds);
    if (item.t() == crow::json::type::Object && item.has("weight")) {
        info.weight = parse_weight(item["weight"]);
    }
    return info;
}

}  // namespace lugbulk::bricklink
