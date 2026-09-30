#include "zip_writer.h"

#include <zlib.h>

#include <cstdint>
#include <ctime>
#include <stdexcept>

namespace lugbulk::zip_writer {

namespace {

void put16(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}
void put32(std::string& out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

}  // namespace

std::string zip(const std::vector<std::pair<std::string, std::string>>& files) {
    // MS-DOS date/time of "now", as zip stores it.
    std::time_t now = std::time(nullptr);
    std::tm t{};
    localtime_r(&now, &t);
    uint32_t dos_time = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2);
    uint32_t dos_date = ((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday;

    std::string out, central;
    for (const auto& [name, data] : files) {
        if (data.size() > 0xFFFFFFFFu || out.size() > 0xFFFFFFFFu) {
            throw std::runtime_error("zip: bundle too large");  // no zip64 here
        }
        uint32_t crc = crc32(0L, reinterpret_cast<const Bytef*>(data.data()),
                             static_cast<uInt>(data.size()));
        uint32_t offset = static_cast<uint32_t>(out.size());
        uint32_t size = static_cast<uint32_t>(data.size());
        const uint32_t kUtf8Names = 1u << 11;  // general-purpose flag: names are UTF-8

        put32(out, 0x04034b50);  // local file header
        put16(out, 20);          // version needed
        put16(out, kUtf8Names);
        put16(out, 0);  // method: stored
        put16(out, dos_time);
        put16(out, dos_date);
        put32(out, crc);
        put32(out, size);
        put32(out, size);
        put16(out, static_cast<uint32_t>(name.size()));
        put16(out, 0);  // extra length
        out += name;
        out += data;

        put32(central, 0x02014b50);  // central directory header
        put16(central, 20);          // version made by
        put16(central, 20);          // version needed
        put16(central, kUtf8Names);
        put16(central, 0);
        put16(central, dos_time);
        put16(central, dos_date);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, static_cast<uint32_t>(name.size()));
        put16(central, 0);  // extra
        put16(central, 0);  // comment
        put16(central, 0);  // disk
        put16(central, 0);  // internal attrs
        put32(central, 0);  // external attrs
        put32(central, offset);
        central += name;
    }
    uint32_t central_offset = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);  // end of central directory
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(files.size()));
    put16(out, static_cast<uint32_t>(files.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, central_offset);
    put16(out, 0);  // comment length
    return out;
}

}  // namespace lugbulk::zip_writer
