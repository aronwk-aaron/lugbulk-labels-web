#include "part_images.h"

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <string_view>
#include <vector>

#include "sheet_pivot.h"

namespace lugbulk::part_images {

namespace {

constexpr long kMissRetrySeconds = 24 * 60 * 60;

// A part photo is ~5 KB; refuse anything absurd.
constexpr size_t kMaxImageBytes = 2 * 1024 * 1024;

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    size_t n = size * nmemb;
    if (out->size() + n > kMaxImageBytes) return 0;  // curl aborts the download
    out->insert(out->end(), ptr, ptr + n);
    return n;
}

bool file_exists_nonempty(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

bool file_exists_empty(const std::string& path, long* mtime_out) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    if (mtime_out) *mtime_out = static_cast<long>(st.st_mtime);
    return st.st_size == 0;
}

// Writes `data` to `path` atomically (temp file + rename), so a concurrent
// request for the same part — or a crash mid-write — never sees a
// half-written image.
bool write_atomically(const std::string& path, const std::vector<uint8_t>& data) {
    static std::atomic<unsigned> counter{0};
    std::string tmp = path + "." + std::to_string(::getpid()) + "." +
                      std::to_string(counter.fetch_add(1)) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out.good()) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

std::optional<std::string> element_id_from_image_name(const std::string& name) {
    constexpr std::string_view kExt = ".jpg";
    if (name.size() <= kExt.size() ||
        name.compare(name.size() - kExt.size(), kExt.size(), kExt) != 0) {
        return std::nullopt;
    }
    std::string id = name.substr(0, name.size() - kExt.size());
    if (!is_valid_element_id(id)) return std::nullopt;
    return id;
}

CachedImage probe_image_cache(const std::string& element_id, const std::string& cache_dir,
                              std::string* path_out) {
    if (path_out) path_out->clear();
    // pivot_sheet only emits digit-only IDs, but this is where the ID
    // becomes a filesystem path — never trust it implicitly.
    if (!is_valid_element_id(element_id)) return CachedImage::kMiss;
    std::string path = cache_dir + "/" + element_id + ".jpg";
    if (path_out) *path_out = path;
    if (file_exists_nonempty(path)) return CachedImage::kHit;
    long mtime = 0;
    if (file_exists_empty(path, &mtime)) {
        long now = static_cast<long>(std::time(nullptr));
        if (now - mtime < kMissRetrySeconds) return CachedImage::kMiss;  // not stale enough to retry yet
    }
    return CachedImage::kUnknown;
}

std::string cached_image_path(const std::string& element_id, const std::string& url,
                              const std::string& cache_dir) {
    std::string path;
    switch (probe_image_cache(element_id, cache_dir, &path)) {
        case CachedImage::kHit: return path;
        case CachedImage::kMiss: return "";
        case CachedImage::kUnknown: break;
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    std::vector<uint8_t> data;
    bool ok = false;
    if (curl) {
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Mozilla/5.0");
        CURLcode rc = curl_easy_perform(curl.get());
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        // Must actually be a JPEG (FF D8 FF) — a CDN error page served with
        // a 200 would otherwise be cached and then fail to embed forever.
        ok = rc == CURLE_OK && status >= 200 && status < 300 && data.size() > 3 &&
             data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    }

    if (ok && write_atomically(path, data)) return path;
    // Cache the miss as an empty file so we don't re-fetch every run.
    std::ofstream(path, std::ios::binary | std::ios::trunc);
    return "";
}

}  // namespace lugbulk::part_images
