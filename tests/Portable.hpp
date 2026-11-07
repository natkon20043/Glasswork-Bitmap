// What the tests need from the operating system, the same on Windows and Linux.

#pragma once

#include "Paths.hpp"

#include <filesystem>
#include <string>

/// This test program's process id, for unique temporary names.
inline int getpid() {
    return gw::currentProcessId();
}

/// A fresh folder under the system's temporary folder.
inline std::filesystem::path temporaryFolder(const std::string& label) {
    std::filesystem::path folder = std::filesystem::temp_directory_path() / (label + "-" + std::to_string(getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder, ignored);
    return folder;
}

/// A path as text with "/" separators.
inline std::string slashed(const std::filesystem::path& path) {
    return path.generic_string();
}

#ifdef _WIN32
#include <cstdlib>
inline int setenv(const char* name, const char* value, int) {
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#endif

/// Deletes a folder and everything in it, read-only files too (git makes its objects read-only,
/// which stops Windows from deleting them).
inline void removeTree(const std::filesystem::path& folder) {
    std::error_code ignored;
    for (auto entry = std::filesystem::recursive_directory_iterator(folder, ignored);
         entry != std::filesystem::recursive_directory_iterator(); entry.increment(ignored)) {
        std::filesystem::permissions(entry->path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add,
                                     ignored);
    }
    std::filesystem::remove_all(folder, ignored);
}

/// The system's temporary folder as text ("/tmp" on Linux).
inline std::string tempRoot() {
    std::string folder = std::filesystem::temp_directory_path().string();
    while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\')) {
        folder.pop_back();
    }
    return folder;
}
