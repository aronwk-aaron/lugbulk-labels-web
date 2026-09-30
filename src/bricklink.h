// Part weights (and BrickLink color names) from the BrickLink API — port
// of lugbulk-label's bricklink.py.
//
// For each LEGO element ID: GET /item_mapping/{element_id} gives the
// BrickLink part number and color, then GET /items/PART/{no} the catalog
// weight in grams. main.cpp caches results (misses too) in SQLite, so each
// part costs two calls once (BrickLink allows 5,000 calls a day).
//
// Needs OAuth 1.0 credentials from
// https://www.bricklink.com/v2/api/register_consumer.page — consumer key
// and secret, plus an access token and secret tied to the server's IP.
#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace lugbulk::bricklink {

struct Credentials {
    std::string consumer_key, consumer_secret, token, token_secret;

    bool complete() const {
        return !consumer_key.empty() && !consumer_secret.empty() && !token.empty() &&
               !token_secret.empty();
    }
};

struct PartInfo {
    std::string part_no;           // BrickLink item number, e.g. "3004"; empty if unknown
    std::string color;             // BrickLink color name; empty if unknown
    std::optional<double> weight;  // grams per piece
};

// The API refused the request (bad credentials, wrong IP, quota...) —
// every other request will fail the same way.
struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// OAuth 1.0 HMAC-SHA1 Authorization header value (RFC 5849). `params` are
// the request's query/form parameters, which are part of the signature;
// nonce/timestamp are generated when empty.
std::string oauth_header(const std::string& method, const std::string& url,
                         const Credentials& creds,
                         const std::map<std::string, std::string>& params = {},
                         std::string nonce = "", std::string timestamp = "");

// Looks one element up. A PartInfo with an empty part_no means BrickLink
// doesn't know the element. Throws ApiError for auth/quota problems and
// std::runtime_error for network failures.
PartInfo fetch(const std::string& element_id, const Credentials& creds);

}  // namespace lugbulk::bricklink
