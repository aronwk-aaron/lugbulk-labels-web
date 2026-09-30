// LEGO element photos, cached on disk and shared by every sheet and user
// (the /img/<element id>.jpg route serves them).
#pragma once

#include <optional>
#include <string>

namespace lugbulk::part_images {

// The element id in a photo file name, "<element id>.jpg" (the /img/ route),
// or nullopt unless the id passes is_valid_element_id — so the name can
// only ever map to a cache file and a fixed-host LEGO CDN URL.
std::optional<std::string> element_id_from_image_name(const std::string& name);

// What the image cache already knows about a part photo, without fetching:
// kHit (the photo is at *path_out), kMiss (a recent cached miss: LEGO has
// no photo, don't ask again yet) or kUnknown (needs a download). An
// invalid element id is a kMiss with an empty path.
enum class CachedImage { kHit, kMiss, kUnknown };
CachedImage probe_image_cache(const std::string& element_id, const std::string& cache_dir,
                              std::string* path_out = nullptr);

// The cached photo's path, downloading it from `url` first if needed
// (layout::image_url_for). Empty on failure (no photo, network trouble).
// `cache_dir` holds one file per element id; a failure (404, timeout, not
// a JPEG) is stored as an empty file and retried after 24 h, so a
// transient CDN outage doesn't blank a photo for good.
std::string cached_image_path(const std::string& element_id, const std::string& url,
                              const std::string& cache_dir);

}  // namespace lugbulk::part_images
