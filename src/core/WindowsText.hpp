#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>
#include <string_view>

namespace gw {

// ---- UTF-8 <-> UTF-16 for the Windows API (the rest of the program is UTF-8 throughout)

inline std::wstring widenText(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

inline std::string narrowText(std::wstring_view text) {
    if (text.empty()) {
        return std::string();
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), needed, nullptr, nullptr);
    return narrow;
}

/// The system's own words for an error code from GetLastError.
inline std::string windowsErrorMessage(unsigned long code) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string message;
    if (length > 0 && buffer != nullptr) {
        message = narrowText(std::wstring_view(buffer, length));
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
            message.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message.empty() ? "error " + std::to_string(code) : message;
}

}  // namespace gw

#endif  // _WIN32
