#include "FileBytes.hpp"

#include <cstdio>

namespace gw {

// Files are read into memory on Windows and Linux. Mapping the original file would let another
// program that shortens it while it is open crash the editor (a bus error on Linux, an in-page
// error on Windows), and neither system has macOS's cheap private clones to map instead.

FileBytes::FileBytes(std::string&& text) : owned_(std::move(text)) {}

FileBytes::FileBytes(FileBytes&& other) noexcept
    : owned_(std::move(other.owned_)), mapping_(other.mapping_), mappingSize_(other.mappingSize_) {
    other.mapping_ = nullptr;
    other.mappingSize_ = 0;
}

FileBytes& FileBytes::operator=(FileBytes&& other) noexcept {
    if (this != &other) {
        release();
        owned_ = std::move(other.owned_);
        mapping_ = other.mapping_;
        mappingSize_ = other.mappingSize_;
        other.mapping_ = nullptr;
        other.mappingSize_ = 0;
    }
    return *this;
}

FileBytes::~FileBytes() {
    release();
}

void FileBytes::release() {
    mapping_ = nullptr;
    mappingSize_ = 0;
    owned_.clear();
}

std::optional<FileBytes> FileBytes::load(const std::filesystem::path& path, size_t smallestMappedFile) {
    (void)smallestMappedFile;
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        return std::nullopt;
    }
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
        return std::nullopt;
    }
    std::string contents;
    auto size = std::filesystem::file_size(path, error);
    if (!error && size > 0) {
        contents.reserve(static_cast<size_t>(size));
    }
    char chunk[64 * 1024];
    while (true) {
        size_t count = std::fread(chunk, 1, sizeof chunk, file);
        if (count > 0) {
            contents.append(chunk, count);
        }
        if (count < sizeof chunk) {
            break;
        }
    }
    bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
        return std::nullopt;
    }
    return FileBytes(std::move(contents));
}

}  // namespace gw
