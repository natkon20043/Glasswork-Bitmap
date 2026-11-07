// Glasswork Bitmap: the program's entry. SDL calls these four functions; between events the program
// sleeps (no frames are drawn unless something changed or a caret blinks).

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "App.hpp"
#include "Loop.hpp"
#include "MainWindow.hpp"
#include "Paths.hpp"
#include "Services.hpp"

#include <filesystem>
#include <string>
#include <vector>

#ifndef GLASSWORK_VERSION
#define GLASSWORK_VERSION "1.0.0"
#endif

using namespace gw;
using namespace gw::app;

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    (void)state;
    SDL_SetAppMetadata("Glasswork Bitmap", GLASSWORK_VERSION, "dev.glasswork.bitmap");
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Glasswork Bitmap", SDL_GetError(), nullptr);
        return SDL_APP_FAILURE;
    }
    initializeLoop();
    Services::shared().start();

    std::string folder;
    std::vector<std::string> files;
    for (int index = 1; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument == "--version" || argument == "-v") {
            SDL_Log("Glasswork Bitmap %s", GLASSWORK_VERSION);
            return SDL_APP_SUCCESS;
        }
        if (argument.empty() || argument[0] == '-') {
            continue;
        }
        std::error_code error;
        std::filesystem::path path = std::filesystem::absolute(pathFromUtf8(argument), error);
        if (std::filesystem::is_directory(path, error)) {
            if (folder.empty()) {
                folder = utf8FromPath(path);
            } else {
                App::shared().openWindow(utf8FromPath(path), {});
            }
        } else {
            files.push_back(utf8FromPath(path));
        }
    }
    if (folder.empty() && files.empty() && environmentValue("GLASSWORK_SCRIPT").empty()) {
        // Pick up where the last session left off.
        nlohmann::json reopen = Services::shared().value("reopenFolders");
        if (reopen.is_array()) {
            bool first = true;
            for (const nlohmann::json& entry : reopen) {
                std::error_code error;
                if (!entry.is_string() || entry.get<std::string>().empty() ||
                    !std::filesystem::is_directory(pathFromUtf8(entry.get<std::string>()), error)) {
                    continue;
                }
                if (first) {
                    folder = entry.get<std::string>();
                    first = false;
                } else {
                    App::shared().openWindow(entry.get<std::string>(), {});
                }
            }
        }
    }
    MainWindow* window = App::shared().openWindow(folder, files);
    std::string script = environmentValue("GLASSWORK_SCRIPT");
    if (!script.empty()) {
        window->runScript(script);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) {
    (void)state;
    if (event->type != wakeEventType()) {
        App::shared().handleEvent(*event);
    }
    return App::shared().running() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

SDL_AppResult SDL_AppIterate(void* state) {
    (void)state;
    return App::shared().iterate() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}

void SDL_AppQuit(void* state, SDL_AppResult result) {
    (void)state;
    (void)result;
}
