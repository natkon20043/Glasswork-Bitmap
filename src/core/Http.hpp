#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace gw {

// ---- Downloads (blocking: call from a background thread)

/// GETs `url`, following redirects. Returns the body, or nothing with `error` saying why.
/// WinHTTP on Windows, libcurl elsewhere.
std::optional<std::string> httpGet(const std::string& url, std::string* error = nullptr);

/// GETs `url` into `file`. False with `error` on failure.
bool httpDownload(const std::string& url, const std::filesystem::path& file, std::string* error = nullptr);

}  // namespace gw
