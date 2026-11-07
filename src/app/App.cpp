#include "App.hpp"

#include "Loop.hpp"
#include "Paths.hpp"
#include "MainWindow.hpp"
#include "Services.hpp"

#include <algorithm>

namespace gw::app {

App& App::shared() {
    static App* app = new App();
    return *app;
}

MainWindow* App::openWindow(const std::string& folder, const std::vector<std::string>& files) {
    windows_.push_back(std::make_unique<MainWindow>(*this, folder, files));
    wakeMainLoop();
    return windows_.back().get();
}

void App::openPathInFrontWindow(const std::string& path) {
    MainWindow* window = frontWindow();
    std::error_code error;
    bool folder = std::filesystem::is_directory(pathFromUtf8(path), error);
    if (!window || (folder && !window->folder().empty())) {
        openWindow(folder ? path : std::string(), folder ? std::vector<std::string>() : std::vector<std::string>{path});
        return;
    }
    if (folder) {
        window->openFolder(path);
    } else {
        window->openPath(path);
    }
}

MainWindow* App::windowFor(SDL_WindowID id) const {
    for (const auto& window : windows_) {
        if (window->windowId() == id) {
            return window.get();
        }
    }
    return nullptr;
}

MainWindow* App::frontWindow() const {
    SDL_Window* focused = SDL_GetKeyboardFocus();
    if (focused) {
        if (MainWindow* window = windowFor(SDL_GetWindowID(focused))) {
            return window;
        }
    }
    return windows_.empty() ? nullptr : windows_.back().get();
}

void App::closeWindow(MainWindow* window) {
    auto found = std::find_if(windows_.begin(), windows_.end(), [window](const auto& candidate) { return candidate.get() == window; });
    if (found == windows_.end()) {
        return;
    }
    // The last window's folder is reopened next time.
    if (windows_.size() == 1 && !quitting_) {
        Services::shared().setValue("reopenFolders", nlohmann::json::array({window->folder()}));
    }
    SDL_HideWindow(window->sdlWindow());
    retired_.push_back(std::move(*found));
    windows_.erase(found);
    if (windows_.empty()) {
        finished_ = true;
    }
    wakeMainLoop();
}

void App::quit() {
    if (quitting_) {
        return;
    }
    quitting_ = true;
    nlohmann::json folders = nlohmann::json::array();
    for (const auto& window : windows_) {
        folders.push_back(window->folder());
    }
    Services::shared().setValue("reopenFolders", folders);
    quitNext(0);
}

void App::quitNext(size_t index) {
    if (windows_.empty()) {
        finished_ = true;
        wakeMainLoop();
        return;
    }
    MainWindow* window = windows_.front().get();
    window->requestClose([this, window](bool ok) {
        if (!ok) {
            quitting_ = false;  // the person chose to stay
            return;
        }
        closeWindow(window);
        quitNext(0);
    });
    (void)index;
}

void App::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_QUIT) {
        quit();
        return;
    }
    SDL_Window* target = SDL_GetWindowFromEvent(&event);
    MainWindow* window = target ? windowFor(SDL_GetWindowID(target)) : nullptr;
    if (!window && event.type == SDL_EVENT_DROP_FILE) {
        window = frontWindow();
    }
    if (window) {
        window->handleEvent(event);
    }
}

bool App::iterate() {
    runPendingWork();
    retired_.clear();
    double next = -1;
    for (const auto& window : windows_) {
        double wanted = window->present();
        if (wanted >= 0 && (next < 0 || wanted < next)) {
            next = wanted;
        }
    }
    if (next >= 0) {
        double deadline = now() + next;
        if (frameTimer_ == 0 || frameDeadline_ < 0 || deadline < frameDeadline_ - 0.001 || now() >= frameDeadline_) {
            if (frameTimer_) {
                cancelTimer(frameTimer_);
            }
            frameDeadline_ = deadline;
            frameTimer_ = afterDelay(next, [this] {
                frameTimer_ = 0;
                frameDeadline_ = -1;
            });
        }
    }
    runPendingWork();  // arms the wake-up for the next timer
    return !finished_;
}

}  // namespace gw::app
