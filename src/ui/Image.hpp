#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gw::ui {

/// Whether a file name looks like a picture the image tab can show.
bool isImageFile(const std::filesystem::path& path);

/// Reads a PNG, JPEG, GIF (first frame), BMP, TGA or PSD file as RGBA pixels.
bool loadImage(const std::filesystem::path& path, std::vector<uint8_t>& rgba, int& width, int& height, std::string* error);

/// Writes 0xAARRGGBB pixels (a Canvas) as a PNG file, uncompressed. For screenshots in tests.
bool savePng(const std::filesystem::path& path, const uint32_t* argb, int width, int height);

}  // namespace gw::ui
