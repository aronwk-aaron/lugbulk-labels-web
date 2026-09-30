// Writes a .zip archive in memory — used for the "Download all" bundle.
// Entries are stored uncompressed: the PDFs inside are already compressed,
// and the CSVs are tiny, so deflating wouldn't save anything worthwhile.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lugbulk::zip_writer {

// (file name inside the zip, contents) -> the .zip file's bytes.
std::string zip(const std::vector<std::pair<std::string, std::string>>& files);

}  // namespace lugbulk::zip_writer
