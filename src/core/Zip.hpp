#pragma once

#include <filesystem>
#include <string>

namespace gw {

// ---- Archives

/// Unpacks a zip archive (such as a .vsix extension) into `destination`, which is created.
/// Handles stored and deflated entries; entries trying to escape the folder ("../") are refused.
bool unzipArchive(const std::filesystem::path& archive, const std::filesystem::path& destination, std::string* error = nullptr);

}  // namespace gw
