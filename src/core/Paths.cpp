#include "Paths.hpp"

#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include "WindowsText.hpp"
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace gw {

std::filesystem::path pathFromUtf8(std::string_view text) {
#ifdef _WIN32
    return std::filesystem::path(widenText(text));
#else
    return std::filesystem::path(std::string(text));
#endif
}

std::string utf8FromPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return narrowText(path.native());
#else
    return path.native();
#endif
}

std::string genericUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return narrowText(path.generic_wstring());
#else
    return path.generic_string();
#endif
}

std::string environmentValue(const char* name) {
#ifdef _WIN32
    std::wstring wideName = widenText(name);
    DWORD needed = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (needed == 0) {
        return std::string();
    }
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), needed);
    value.resize(written);
    return narrowText(value);
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::string homeFolder() {
#ifdef _WIN32
    std::string profile = environmentValue("USERPROFILE");
    if (!profile.empty()) {
        return profile;
    }
    return environmentValue("HOMEDRIVE") + environmentValue("HOMEPATH");
#else
    std::string home = environmentValue("HOME");
    return home.empty() ? std::string("/") : home;
#endif
}

std::filesystem::path supportFolder() {
    static const std::filesystem::path folder = [] {
        std::string overridden = environmentValue("GLASSWORK_SUPPORT_FOLDER");
        if (!overridden.empty()) {
            return pathFromUtf8(overridden);
        }
#ifdef _WIN32
        std::string appData = environmentValue("APPDATA");
        if (appData.empty()) {
            appData = homeFolder() + "\\AppData\\Roaming";
        }
        return pathFromUtf8(appData) / L"Glasswork Bitmap";
#else
        std::string config = environmentValue("XDG_CONFIG_HOME");
        if (config.empty()) {
            config = homeFolder() + "/.config";
        }
        return std::filesystem::path(config) / "glasswork-bitmap";
#endif
    }();
    return folder;
}

std::filesystem::path programFolder() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#else
    std::error_code error;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::current_path(error) : self.parent_path();
#endif
}

std::string abbreviateHome(const std::string& path) {
    std::string home = homeFolder();
    if (!home.empty() && path.size() >= home.size() && path.compare(0, home.size(), home) == 0 &&
        (path.size() == home.size() || path[home.size()] == '/' || path[home.size()] == '\\')) {
        return "~" + path.substr(home.size());
    }
    return path;
}

int currentProcessId() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

}  // namespace gw
