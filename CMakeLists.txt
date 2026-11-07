cmake_minimum_required(VERSION 3.24)
project(GlassworkBitmap VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GLASSWORK_BUILD_TESTS "Build the core's test programs" ON)
option(GLASSWORK_STATIC_RUNTIME "Windows: link the C++ runtime and SDL statically (one self-contained .exe)" ON)

include(FetchContent)

# JSON: extension manifests, grammars and themes.
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

# Oniguruma: the regex engine TextMate grammars are written for. Built statically.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCUMENTATION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(oniguruma
    GIT_REPOSITORY https://github.com/kkos/oniguruma.git
    GIT_TAG v6.9.10
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(oniguruma)

# SDL3: the window, keyboard, mouse, clipboard and file dialogs. Everything inside the window
# is drawn by the program itself, pixel by pixel.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not installed: building it from source")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    if(MINGW AND GLASSWORK_STATIC_RUNTIME)
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
endif()

# ---------------------------------------------------------------------------
# Core: plain C++20, no UI. Text model, TextMate engine, themes, extensions, terminal, LSP, DAP, git.
set(GLASSWORK_CORE_SOURCES
    src/core/Paths.cpp
    src/core/Http.cpp
    src/core/Zip.cpp
    src/core/FileBytes.cpp
    src/core/LineStore.cpp
    src/core/TextBuffer.cpp
    src/core/Document.cpp
    src/core/CharacterWidth.cpp
    src/core/Wrap.cpp
    src/core/RowMap.cpp
    src/core/Folding.cpp
    src/core/EditorConfig.cpp
    src/core/SemanticTokens.cpp
    src/core/Tasks.cpp
    src/core/Snippet.cpp
    src/core/SnippetLibrary.cpp
    src/core/NavigationHistory.cpp
    src/core/WordSuggestions.cpp
    src/core/TextTransforms.cpp
    src/core/SelectionExpand.cpp
    src/core/Dap.cpp
    src/core/Highlighter.cpp
    src/core/Jsonc.cpp
    src/core/SettingsLookup.cpp
    src/core/textmate/Regex.cpp
    src/core/textmate/FirstBytes.cpp
    src/core/textmate/Grammar.cpp
    src/core/textmate/Tokenizer.cpp
    src/core/Theme.cpp
    src/core/Styling.cpp
    src/core/EditorSettings.cpp
    src/core/Sections.cpp
    src/core/Fuzzy.cpp
    src/core/CssColor.cpp
    src/core/Glob.cpp
    src/core/IconTheme.cpp
    src/core/Keybindings.cpp
    src/core/CompletionRank.cpp
    src/core/LanguageServer.cpp
    src/core/LspServer.cpp
    src/core/ExtensionHost.cpp
    src/core/ProcessPosix.cpp
    src/core/ProcessWindows.cpp
    src/core/LineDiff.cpp
    src/core/Git.cpp
    src/core/Markdown.cpp
    src/core/ProjectSearch.cpp
    src/core/Terminal.cpp
    src/core/PtyPosix.cpp
    src/core/PtyWindows.cpp
    src/core/RemoteHost.cpp
    src/core/Vim.cpp
    src/core/CursorSmear.cpp
    src/core/PixelFont.cpp
    src/core/BitmapMode.cpp
    src/core/Extensions.cpp)

add_library(glasswork_core STATIC ${GLASSWORK_CORE_SOURCES})
target_include_directories(glasswork_core PUBLIC src/core third_party)
target_link_libraries(glasswork_core PUBLIC nlohmann_json::nlohmann_json onig)
if(WIN32)
    target_link_libraries(glasswork_core PUBLIC winhttp)
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(glasswork_core PUBLIC CURL::libcurl Threads::Threads util)
endif()
target_compile_options(glasswork_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# Tests (ctest runs them all)
if(GLASSWORK_BUILD_TESTS)
    enable_testing()
    function(glasswork_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE glasswork_core)
        target_include_directories(${name} PRIVATE tests)
        target_compile_definitions(${name} PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    glasswork_test(core_tests tests/core_tests.cpp)
    glasswork_test(layout_tests tests/layout_tests.cpp)
    glasswork_test(vim_tests tests/vim_tests.cpp)
    glasswork_test(terminal_tests tests/terminal_tests.cpp)
    glasswork_test(cursor_smear_tests tests/cursor_smear_tests.cpp)
    glasswork_test(bitmap_tests tests/bitmap_tests.cpp)
    glasswork_test(markdown_tests tests/markdown_tests.cpp)
    glasswork_test(search_tests tests/search_tests.cpp)
    glasswork_test(language_protocol_tests tests/language_server_tests.cpp)
    glasswork_test(git_tests tests/git_tests.cpp)
    glasswork_test(lsp_server_tests tests/lsp_server_tests.cpp)
    glasswork_test(dap_tests tests/dap_tests.cpp)
    glasswork_test(pty_tests tests/pty_tests.cpp)
    glasswork_test(remote_tests tests/remote_tests.cpp)
    glasswork_test(extension_host_tests tests/extension_host_tests.cpp)
    find_program(GLASSWORK_NODE node)
    if(GLASSWORK_NODE)
        add_test(NAME extension_host_js COMMAND ${GLASSWORK_NODE} ${CMAKE_SOURCE_DIR}/tests/host/run.js)
    endif()
endif()

# ---------------------------------------------------------------------------
# The app: an SDL window whose every pixel is drawn by src/ui and src/app.
file(GLOB GLASSWORK_APP_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp src/app/*.cpp)
list(FILTER GLASSWORK_APP_SOURCES EXCLUDE REGEX ".*/main\.cpp$")
add_library(glasswork_app STATIC ${GLASSWORK_APP_SOURCES})
target_include_directories(glasswork_app PUBLIC src/ui src/app)
target_compile_definitions(glasswork_app PRIVATE GLASSWORK_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                           GLASSWORK_VERSION="${PROJECT_VERSION}")
if(TARGET SDL3::SDL3-static AND (WIN32 OR NOT TARGET SDL3::SDL3))
    target_link_libraries(glasswork_app PUBLIC glasswork_core SDL3::SDL3-static)
else()
    target_link_libraries(glasswork_app PUBLIC glasswork_core SDL3::SDL3)
endif()
if(WIN32)
    target_link_libraries(glasswork_app PUBLIC dwmapi shell32 ole32 uuid)
endif()
target_compile_options(glasswork_app PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

set(GLASSWORK_APP_RESOURCES)
if(WIN32)
    set(GLASSWORK_APP_RESOURCES packaging/windows/glasswork-bitmap.rc)
endif()
add_executable(glasswork-bitmap WIN32 src/app/main.cpp ${GLASSWORK_APP_RESOURCES})
target_link_libraries(glasswork-bitmap PRIVATE glasswork_app)
target_compile_definitions(glasswork-bitmap PRIVATE GLASSWORK_VERSION="${PROJECT_VERSION}")
set_target_properties(glasswork-bitmap PROPERTIES OUTPUT_NAME "glasswork-bitmap")
if(MINGW)
    target_link_options(glasswork-bitmap PRIVATE -municode)  # SDL's entry is wWinMain under UNICODE
endif()
# The extension host's JavaScript sits next to the program.
add_custom_command(TARGET glasswork-bitmap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/host $<TARGET_FILE_DIR:glasswork-bitmap>/host)
