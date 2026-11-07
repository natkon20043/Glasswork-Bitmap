#include "Image.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>

#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace gw::ui {

bool isImageFile(const std::filesystem::path& path) {
    static const std::set<std::string> extensions = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".psd", ".pic", ".pnm", ".ppm", ".pgm"};
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extensions.count(extension) > 0;
}

bool loadImage(const std::filesystem::path& path, std::vector<uint8_t>& rgba, int& width, int& height, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) {
            *error = "The file couldn't be read.";
        }
        return false;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) {
        if (error) {
            *error = std::string("This picture can't be shown: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown format");
        }
        return false;
    }
    rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

namespace {

uint32_t crc32Of(const uint8_t* data, size_t length, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void putBig(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void chunk(std::vector<uint8_t>& out, const char* kind, const std::vector<uint8_t>& data) {
    putBig(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(kind, kind + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    putBig(out, crc32Of(body.data(), body.size()));
}

}  // namespace

bool savePng(const std::filesystem::path& path, const uint32_t* argb, int width, int height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < width; ++x) {
            uint32_t pixel = argb[static_cast<size_t>(y) * width + x];
            raw.push_back(static_cast<uint8_t>(pixel >> 16));
            raw.push_back(static_cast<uint8_t>(pixel >> 8));
            raw.push_back(static_cast<uint8_t>(pixel));
        }
    }
    // zlib stream of stored (uncompressed) blocks.
    std::vector<uint8_t> zlib = {0x78, 0x01};
    size_t offset = 0;
    while (offset < raw.size() || raw.empty()) {
        size_t length = std::min<size_t>(65535, raw.size() - offset);
        bool last = offset + length >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(length));
        zlib.push_back(static_cast<uint8_t>(length >> 8));
        zlib.push_back(static_cast<uint8_t>(~length));
        zlib.push_back(static_cast<uint8_t>(~length >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<long>(offset), raw.begin() + static_cast<long>(offset + length));
        offset += length;
        if (raw.empty()) {
            break;
        }
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    putBig(zlib, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> header;
    putBig(header, static_cast<uint32_t>(width));
    putBig(header, static_cast<uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", zlib);
    chunk(out, "IEND", {});
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(file);
}

}  // namespace gw::ui
