#include "Platform.hpp"

#include "Loop.hpp"
#include "Paths.hpp"
#include "Process.hpp"

#include <SDL3/SDL.h>

#include <filesystem>

#ifdef _WIN32
#include "WindowsText.hpp"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>
#endif

namespace gw::app {

std::string clipboardText() {
    char* text = SDL_GetClipboardText();
    std::string result = text != nullptr ? text : "";
    SDL_free(text);
    // The rest of the program uses "\n" line breaks; the document converts to the file's own.
    std::string cleaned;
    cleaned.reserve(result.size());
    for (size_t index = 0; index < result.size(); ++index) {
        if (result[index] == '\r' && index + 1 < result.size() && result[index + 1] == '\n') {
            continue;
        }
        cleaned += result[index];
    }
    return cleaned;
}

void setClipboardText(const std::string& text) {
#ifdef _WIN32
    // Windows programs expect "\r\n" between lines on the clipboard.
    std::string windows;
    windows.reserve(text.size() + text.size() / 32);
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\n' && (index == 0 || text[index - 1] != '\r')) {
            windows += '\r';
        }
        windows += text[index];
    }
    SDL_SetClipboardText(windows.c_str());
#else
    SDL_SetClipboardText(text.c_str());
#endif
}

void openExternally(const std::string& target) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", widenText(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string program = target;
    runInBackground([program] { runProcess({"xdg-open", program}, {}); });
#endif
}

void revealInFileManager(const std::string& path) {
#ifdef _WIN32
    std::wstring arguments = L"/select,\"" + widenText(path) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
#else
    std::error_code error;
    std::filesystem::path target(path);
    std::string folder = std::filesystem::is_directory(target, error) ? path : target.parent_path().string();
    runInBackground([folder] { runProcess({"xdg-open", folder}, {}); });
#endif
}

bool moveToTrash(const std::string& path, std::string* error) {
#ifdef _WIN32
    std::wstring from = widenText(path);
    from.push_back(L'\0');  // the list ends with an extra NUL
    SHFILEOPSTRUCTW operation = {};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    int result = SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        if (error) {
            *error = "Windows couldn't move it to the Recycle Bin (error " + std::to_string(result) + ").";
        }
        return false;
    }
    return true;
#else
    ProcessResult result = runProcess({"gio", "trash", path}, {});
    if (result.succeeded()) {
        return true;
    }
    if (error) {
        *error = result.started ? result.errorOutput : "gio isn't installed, so files can't be moved to the trash.";
    }
    return false;
#endif
}

namespace {

struct DialogRequest {
    std::function<void(std::vector<std::string>)> done;
};

void SDLCALL dialogFinished(void* userdata, const char* const* files, int) {
    auto* request = static_cast<DialogRequest*>(userdata);
    std::vector<std::string> chosen;
    if (files != nullptr) {
        for (const char* const* file = files; *file != nullptr; ++file) {
            chosen.emplace_back(*file);
        }
    }
    runOnMain([request, chosen] {
        request->done(chosen);
        delete request;
    });
}

}  // namespace

void chooseFolder(SDL_Window* window, const std::string& start, std::function<void(std::optional<std::string>)> done) {
    auto* request = new DialogRequest{[done](std::vector<std::string> files) {
        done(files.empty() ? std::nullopt : std::optional<std::string>(files.front()));
    }};
    SDL_ShowOpenFolderDialog(dialogFinished, request, window, start.empty() ? nullptr : start.c_str(), false);
}

void chooseFiles(SDL_Window* window, const std::string& start, std::function<void(std::vector<std::string>)> done) {
    auto* request = new DialogRequest{std::move(done)};
    SDL_ShowOpenFileDialog(dialogFinished, request, window, nullptr, 0, start.empty() ? nullptr : start.c_str(), true);
}

void chooseSaveLocation(SDL_Window* window, const std::string& suggested, std::function<void(std::optional<std::string>)> done) {
    auto* request = new DialogRequest{[done](std::vector<std::string> files) {
        done(files.empty() ? std::nullopt : std::optional<std::string>(files.front()));
    }};
    SDL_ShowSaveFileDialog(dialogFinished, request, window, nullptr, 0, suggested.empty() ? nullptr : suggested.c_str());
}

void beep() {
#ifdef _WIN32
    MessageBeep(MB_OK);
#endif
}

void useDarkTitleBar(SDL_Window* window) {
#ifdef _WIN32
    SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    HWND handle = static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (handle != nullptr) {
        BOOL dark = TRUE;
        constexpr DWORD kUseImmersiveDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(handle, kUseImmersiveDarkMode, &dark, sizeof dark);
    }
#else
    (void)window;
#endif
}

}  // namespace gw::app
