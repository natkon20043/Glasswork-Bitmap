#include "Zip.hpp"
#include "Paths.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

// stb_image carries a small, well-tested inflate; only that part is used here.
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_PNG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace gw {

namespace {

uint16_t read16(const unsigned char* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read32(const unsigned char* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) | (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

/// A relative name inside the archive that stays inside the folder it is unpacked into.
bool isSafeEntryName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) {
        return false;
    }
    std::stringstream parts(name);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part == "..") {
            return false;
        }
    }
    return name.find('\\') == std::string::npos || name.find("..\\") == std::string::npos;
}

}  // namespace

bool unzipArchive(const std::filesystem::path& archive, const std::filesystem::path& destination, std::string* error) {
    auto fail = [error](const std::string& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    std::ifstream file(archive, std::ios::binary);
    if (!file) {
        return fail("couldn't open the archive");
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 22) {
        return fail("not a zip archive");
    }
    // The end-of-central-directory record is in the last 64 KB (after an optional comment).
    size_t end = std::string::npos;
    size_t searchFrom = bytes.size() >= 22 + 65535 ? bytes.size() - 22 - 65535 : 0;
    for (size_t index = bytes.size() - 22 + 1; index-- > searchFrom;) {
        if (read32(&bytes[index]) == 0x06054b50) {
            end = index;
            break;
        }
    }
    if (end == std::string::npos) {
        return fail("not a zip archive");
    }
    uint16_t entries = read16(&bytes[end + 10]);
    uint32_t directoryOffset = read32(&bytes[end + 16]);
    std::error_code code;
    std::filesystem::create_directories(destination, code);

    size_t cursor = directoryOffset;
    for (uint16_t entry = 0; entry < entries; ++entry) {
        if (cursor + 46 > bytes.size() || read32(&bytes[cursor]) != 0x02014b50) {
            return fail("the archive's directory is damaged");
        }
        uint16_t method = read16(&bytes[cursor + 10]);
        uint32_t compressedSize = read32(&bytes[cursor + 20]);
        uint32_t size = read32(&bytes[cursor + 24]);
        uint16_t nameLength = read16(&bytes[cursor + 28]);
        uint16_t extraLength = read16(&bytes[cursor + 30]);
        uint16_t commentLength = read16(&bytes[cursor + 32]);
        uint32_t localOffset = read32(&bytes[cursor + 42]);
        if (cursor + 46 + nameLength > bytes.size()) {
            return fail("the archive's directory is damaged");
        }
        std::string name(reinterpret_cast<const char*>(&bytes[cursor + 46]), nameLength);
        cursor += 46 + nameLength + extraLength + commentLength;

        if (!isSafeEntryName(name)) {
            return fail("the archive has an unsafe file name: " + name);
        }
        std::filesystem::path target = destination / pathFromUtf8(name);
        if (name.back() == '/') {
            std::filesystem::create_directories(target, code);
            continue;
        }
        if (localOffset + 30 > bytes.size() || read32(&bytes[localOffset]) != 0x04034b50) {
            return fail("the archive is damaged at " + name);
        }
        size_t dataStart = localOffset + 30 + read16(&bytes[localOffset + 26]) + read16(&bytes[localOffset + 28]);
        if (dataStart + compressedSize > bytes.size()) {
            return fail("the archive is cut short at " + name);
        }
        std::filesystem::create_directories(target.parent_path(), code);
        std::ofstream out(target, std::ios::binary);
        if (!out) {
            return fail("couldn't write " + name);
        }
        if (method == 0) {
            out.write(reinterpret_cast<const char*>(&bytes[dataStart]), compressedSize);
        } else if (method == 8) {
            std::vector<char> inflated(std::max<size_t>(size, 1));
            int produced = stbi_zlib_decode_noheader_buffer(inflated.data(), static_cast<int>(inflated.size()),
                                                            reinterpret_cast<const char*>(&bytes[dataStart]),
                                                            static_cast<int>(compressedSize));
            if (produced < 0 || static_cast<uint32_t>(produced) != size) {
                return fail("couldn't unpack " + name);
            }
            out.write(inflated.data(), produced);
        } else {
            return fail("the archive uses a compression this can't unpack (" + std::to_string(method) + ")");
        }
        if (!out) {
            return fail("couldn't write " + name);
        }
    }
    return true;
}

}  // namespace gw
