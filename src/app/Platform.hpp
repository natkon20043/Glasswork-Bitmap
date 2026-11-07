#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace gw::app {

// ---- Clipboard
std::string clipboardText();
void setClipboardText(const std::string& text);

// ---- The system's file manager and browser
/// Opens a web address, or a file with the program the system uses for it.
void openExternally(const std::string& target);
/// Shows the file selected in Explorer (Windows) or opens its folder (Linux).
void revealInFileManager(const std::string& path);
/// Moves a file or folder to the Recycle Bin / Trash. False with `error` if it couldn't.
bool moveToTrash(const std::string& path, std::string* error);

// ---- File dialogs (native; the answer arrives on the main thread)
void chooseFolder(SDL_Window* window, const std::string& start, std::function<void(std::optional<std::string>)> done);
void chooseFiles(SDL_Window* window, const std::string& start, std::function<void(std::vector<std::string>)> done);
void chooseSaveLocation(SDL_Window* window, const std::string& suggested, std::function<void(std::optional<std::string>)> done);

// ---- Feedback
void beep();

// ---- Window looks
/// A dark title bar on Windows 10 and 11, to sit well with the dark screen below it.
void useDarkTitleBar(SDL_Window* window);

}  // namespace gw::app
