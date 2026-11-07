#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace gw {

// ---- Text <-> paths
/// Paths are UTF-8 text everywhere in the program; these convert without going through the
/// system's legacy code page (which on Windows can't hold every name).
std::filesystem::path pathFromUtf8(std::string_view text);
std::string utf8FromPath(const std::filesystem::path& path);
/// With "/" separators, as language servers, git and the file lists expect.
std::string genericUtf8(const std::filesystem::path& path);

// ---- Folders
/// The user's home folder.
std::string homeFolder();
/// Where Glasswork Bitmap keeps its settings, extensions and state:
/// %APPDATA%\Glasswork Bitmap on Windows, $XDG_CONFIG_HOME/glasswork-bitmap (~/.config/...) on Linux.
std::filesystem::path supportFolder();
/// The folder the running program is in.
std::filesystem::path programFolder();

// ---- Environment
/// Empty when unset.
std::string environmentValue(const char* name);
/// "~/x" for paths inside the home folder.
std::string abbreviateHome(const std::string& path);

/// This program's process id.
int currentProcessId();

}  // namespace gw
