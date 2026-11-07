#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <vector>

namespace gw::app {

class MainWindow;

/// The program: its windows, and quitting.
class App {
public:
    static App& shared();

    /// A new window on a folder (or none) with some files open.
    MainWindow* openWindow(const std::string& folder, const std::vector<std::string>& files);
    /// Opens a path from the command line or a drop: a folder gets a window of its own unless the
    /// front window has none yet.
    void openPathInFrontWindow(const std::string& path);
    /// Closes a window once it has asked about unsaved files.
    void closeWindow(MainWindow* window);
    /// Asks every window about unsaved files, then ends the program.
    void quit();

    void handleEvent(const SDL_Event& event);
    /// Draws the windows; true while the program should keep running.
    bool iterate();
    bool running() const { return !finished_; }
    MainWindow* frontWindow() const;
    const std::vector<std::unique_ptr<MainWindow>>& windows() const { return windows_; }

private:
    MainWindow* windowFor(SDL_WindowID id) const;
    void quitNext(size_t index);

    std::vector<std::unique_ptr<MainWindow>> windows_;
    std::vector<std::unique_ptr<MainWindow>> retired_;
    uint64_t frameTimer_ = 0;
    double frameDeadline_ = -1;
    bool quitting_ = false;
    bool finished_ = false;
};

}  // namespace gw::app
