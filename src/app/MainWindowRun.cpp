#include "MainWindow.hpp"

#include "Loop.hpp"
#include "Paths.hpp"
#include "Platform.hpp"
#include "Process.hpp"
#include "Services.hpp"
#include "Tasks.hpp"

#include <algorithm>
#include <sstream>

namespace gw::app {

namespace fs = std::filesystem;

namespace {

ShellKind shellKindOf(const std::string& shell) {
    return shell.empty() ? defaultShellKind() : shellKindFor(shell);
}

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    char quote = 0;
    for (char character : text) {
        if (quote) {
            if (character == quote) {
                quote = 0;
            } else {
                current += character;
            }
        } else if (character == '"' || character == '\'') {
            quote = character;
        } else if (character == ' ' || character == '\t') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else {
            current += character;
        }
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

}  // namespace

// ---------------------------------------------------------------------------
// The terminal

void MainWindow::setTerminalVisible(bool visible) {
    terminal_.shell = Services::shared().settings().terminalShell;
    if (terminal_.projectFolder.empty()) {
        terminal_.projectFolder = folder_.empty() ? homeFolder() : folder_;
    }
    if (visible == terminalVisible_) {
        if (visible) {
            setFocus(Focus::Terminal);
        }
        return;
    }
    terminalVisible_ = visible;
    layout();
    if (visible) {
        if (terminal_.count() == 0) {
            terminal_.newTerminal();
        }
        setFocus(Focus::Terminal);
    } else if (focus_ == Focus::Terminal) {
        setFocus(Focus::Editor);
    }
    dirty_ = true;
}

void MainWindow::runTaskCommand(const std::string& command) {
    if (terminal_.count() == 0) {
        // The task's own terminal is the first one: don't start an idle shell beside it.
        terminal_.shell = Services::shared().settings().terminalShell;
        terminal_.projectFolder = folder_.empty() ? homeFolder() : folder_;
        terminalVisible_ = true;
        layout();
    } else {
        setTerminalVisible(true);
    }
    ShellKind kind = shellKindOf(terminal_.shell);
    std::string full = command;
    if (!folder_.empty()) {
        std::string change = (kind == ShellKind::Cmd ? "cd /d " : "cd ") + quoteForShell(folder_, kind);
        full = thenCommand(change, command, kind);
    }
    terminal_.runTaskCommand(full);
    setFocus(Focus::Terminal);
}

void MainWindow::runTask() {
    if (folder_.empty()) {
        showError("Open a folder first", "Tasks come from the build files in your project folder.");
        return;
    }
    ShellKind kind = shellKindOf(Services::shared().settings().terminalShell);
    std::vector<Task> tasks = detectTasks(pathFromUtf8(folder_), kind);
    if (tasks.empty()) {
        showError("No tasks found", "Glasswork Bitmap looks for CMakeLists.txt, Cargo.toml, go.mod, package.json, *.sln, *.csproj and "
                                    "Makefiles in the project folder.");
        return;
    }
    std::vector<PaletteItem> items;
    for (const Task& task : tasks) {
        PaletteItem item;
        item.title = task.label;
        const char* kindName = task.kind == Task::Kind::Build ? "build" : task.kind == Task::Kind::Run ? "run" : task.kind == Task::Kind::Test ? "test" : "task";
        item.subtitle = std::string(kindName) + " - " + task.command;
        std::string command = task.command;
        item.run = [this, command] {
            saveAllQuietly();
            runTaskCommand(command);
        };
        items.push_back(std::move(item));
    }
    showPaletteItems(std::move(items), "Run task...");
}

void MainWindow::buildProject() {
    if (!folder_.empty()) {
        ShellKind kind = shellKindOf(Services::shared().settings().terminalShell);
        for (const Task& task : detectTasks(pathFromUtf8(folder_), kind)) {
            if (task.kind == Task::Kind::Build) {
                saveAllQuietly();
                runTaskCommand(task.command);
                return;
            }
        }
    }
    runTask();
}

void MainWindow::runCurrentFile() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        showError("Nothing to run", "Save the file first.");
        return;
    }
    ShellKind kind = shellKindOf(Services::shared().settings().terminalShell);
    std::string command = commandToRunFile(pathFromUtf8(tab->path()), tab->document->languageId, kind);
    if (command.empty()) {
        showError("Can't run this file", "Glasswork Bitmap can run scripts (Python, JavaScript, PowerShell, shell, Ruby, Go...) and "
                                         "single C, C++ and Rust files.");
        return;
    }
    if (tab->dirty()) {
        saveTab(*tab);
    }
    runTaskCommand(command);
}

// ---------------------------------------------------------------------------
// Debugging

void MainWindow::setUpDebugger() {
    debugger_.onChange = [this] {
        debugPanel_->reload();
        if (debugger_.state() != DebugState::Stopped) {
            executionLine_ = -1;
            updateExecutionMarker();
        }
        dirty_ = true;
    };
    debugger_.onLocation = [this](const DebugFrame& frame) { showDebugFrame(frame); };
    debugger_.onOutput = [this](const std::string& text, const std::string& category) {
        debugPanel_->appendConsole(text, category);
        dirty_ = true;
    };
    debugger_.onError = [this](const std::string& message) {
        debugPanel_->appendConsole(message + "\n", "stderr");
        showError("Debugger", message);
    };
    debugger_.onEnded = [this](long code) {
        executionLine_ = -1;
        updateExecutionMarker();
        debugPanel_->appendConsole("Debugging ended (exit code " + std::to_string(code) + ").\n", "console");
        dirty_ = true;
    };
}

void MainWindow::updateExecutionMarker() {
    Tab* tab = currentTab();
    bool here = tab && tab->kind == Tab::Kind::Text && !tab->path().empty() && executionLine_ >= 0 && !executionPath_.empty() &&
                tabForPath(executionPath_) == tab;
    editor_.setExecutionLine(here ? executionLine_ : -1);
    dirty_ = true;
}

void MainWindow::showDebugFrame(const DebugFrame& frame) {
    if (frame.path.empty() || frame.line <= 0) {
        return;
    }
    executionPath_ = frame.path;
    executionLine_ = frame.line - 1;
    panelOpen_ = true;
    panel_ = SidePanel::Debug;
    layout();
    debugPanel_->reload();
    std::error_code error;
    if (!fs::exists(pathFromUtf8(frame.path), error)) {
        return;
    }
    openPath(frame.path, frame.line);
    updateExecutionMarker();
    SDL_RaiseWindow(window_);
}

void MainWindow::startDebugging() {
    if (debugger_.state() == DebugState::Stopped) {
        debugger_.resume();
        return;
    }
    if (debugger_.state() != DebugState::Idle) {
        return;
    }
    if (!folder_.empty()) {
        nlohmann::json remembered = Services::shared().value("debugProgram:" + folder_);
        std::error_code error;
        if (remembered.is_string() && fs::is_regular_file(pathFromUtf8(remembered.get<std::string>()), error)) {
            nlohmann::json arguments = Services::shared().value("debugArguments:" + folder_);
            debugPanel_->clearConsole();
            launchDebugProgram(remembered.get<std::string>(), splitWords(arguments.is_string() ? arguments.get<std::string>() : ""));
            return;
        }
    }
    Tab* tab = currentTab();
    if (tab && tab->kind == Tab::Kind::Text && !tab->path().empty()) {
        std::string extension = utf8FromPath(pathFromUtf8(tab->path()).extension());
        const std::string& language = tab->document->languageId;
        if ((language == "c" && extension == ".c") || (language == "cpp" && (extension == ".cpp" || extension == ".cc" || extension == ".cxx"))) {
            debugCurrentFile();
            return;
        }
    }
    debugExecutable();
}

void MainWindow::debugCurrentFile() {
    Tab* tab = currentTab();
    std::string extension = tab && !tab->path().empty() ? utf8FromPath(pathFromUtf8(tab->path()).extension()) : std::string();
    bool isC = tab && tab->document && tab->document->languageId == "c" && extension == ".c";
    bool isCpp = tab && tab->document && tab->document->languageId == "cpp" && (extension == ".cpp" || extension == ".cc" || extension == ".cxx");
    if (!isC && !isCpp) {
        showError("Can't debug this file", "Glasswork Bitmap can build a single C or C++ file for debugging. For other projects, build "
                                           "them first and use Debug > Debug Executable.");
        return;
    }
    saveAllQuietly();
    openSidePanel(SidePanel::Debug);
    debugPanel_->clearConsole();
    std::string compiler = isCpp ? (!findExecutable("clang++").empty() ? "clang++" : "g++") : (!findExecutable("clang").empty() ? "clang" : "gcc");
    std::string compilerPath = findExecutable(compiler);
    if (compilerPath.empty()) {
        showError("No compiler found", "Install a C/C++ compiler (MSYS2's mingw-w64 GCC, or LLVM) and make sure it is on the PATH.");
        return;
    }
    std::error_code error;
    fs::path output = fs::temp_directory_path(error) / ("glasswork-debug-" + std::to_string(currentProcessId()));
#ifdef _WIN32
    output += ".exe";
#endif
    std::vector<std::string> command = {compilerPath};
    if (isCpp) {
        command.push_back("-std=c++20");
    }
    std::string source = tab->path();
    command.insert(command.end(), {"-g", "-O0", "-Wall", source, "-o", utf8FromPath(output)});
    debugPanel_->appendConsole("Building " + utf8FromPath(pathFromUtf8(source).filename()) + "...\n", "console");
    std::weak_ptr<bool> alive = alive_;
    std::string program = utf8FromPath(output);
    runInBackground([this, alive, command, program] {
        auto result = std::make_shared<ProcessResult>(runProcess(command));
        runOnMain([this, alive, result, program] {
            if (alive.expired()) {
                return;
            }
            std::string messages = result->output + result->errorOutput + result->startError;
            if (!messages.empty()) {
                debugPanel_->appendConsole(messages + (messages.back() == '\n' ? "" : "\n"), result->succeeded() ? "console" : "stderr");
            }
            if (!result->succeeded()) {
                showError("The build failed", "The compiler's messages are in the debugger's console.");
                return;
            }
            launchDebugProgram(program, {});
        });
    });
}

void MainWindow::debugExecutable() {
    std::string start = folder_.empty() ? homeFolder() : folder_;
    std::error_code error;
    if (!folder_.empty() && fs::is_directory(pathFromUtf8(folder_) / "build", error)) {
        start = utf8FromPath(pathFromUtf8(folder_) / "build");
    }
    std::weak_ptr<bool> alive = alive_;
    chooseFiles(window_, start, [this, alive](std::vector<std::string> paths) {
        if (alive.expired() || paths.empty()) {
            return;
        }
        std::string program = paths.front();
        DialogOverlay::Options options;
        options.title = "Arguments";
        options.lines = {"Command-line arguments for " + utf8FromPath(pathFromUtf8(program).filename()) + " (leave empty for none)."};
        options.hasField = true;
        nlohmann::json remembered = folder_.empty() ? nlohmann::json() : Services::shared().value("debugArguments:" + folder_);
        options.fieldText = remembered.is_string() ? remembered.get<std::string>() : "";
        options.buttons = {"&Debug", "Cancel"};
        pushOverlay(std::make_unique<DialogOverlay>(options, [this, program](int button, const std::string& text) {
            if (button != 0) {
                return;
            }
            if (!folder_.empty()) {
                Services::shared().setValue("debugProgram:" + folder_, program);
                Services::shared().setValue("debugArguments:" + folder_, text);
            }
            debugPanel_->clearConsole();
            launchDebugProgram(program, splitWords(text));
        }));
    });
}

void MainWindow::launchDebugProgram(const std::string& program, const std::vector<std::string>& arguments) {
    openSidePanel(SidePanel::Debug);
    std::string directory = folder_.empty() ? utf8FromPath(pathFromUtf8(program).parent_path()) : folder_;
    debugger_.startProgram(program, arguments, directory, currentBreakpoints());
}

std::map<std::string, std::set<size_t>> MainWindow::currentBreakpoints() {
    for (auto& tab : tabs_) {
        if (tab->kind == Tab::Kind::Text && !tab->path().empty()) {
            breakpointStore_[tab->path()] = tab->document->breakpointLines;
        }
    }
    std::map<std::string, std::set<size_t>> result;
    for (const auto& [path, lines] : breakpointStore_) {
        if (!lines.empty()) {
            result[path] = lines;
        }
    }
    return result;
}

void MainWindow::breakpointsChanged() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        return;
    }
    std::string path = tab->path();
    breakpointStore_[path] = tab->document->breakpointLines;
    if (breakpointStore_[path].empty()) {
        breakpointStore_.erase(path);
    }
    debugger_.setBreakpointLines(tab->document->breakpointLines, path);
    saveSession();
    dirty_ = true;
}

void MainWindow::toggleBreakpoint() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        beep();
        return;
    }
    editor_.toggleBreakpointAtLine(tab->document->selection.caret.line);
}

void MainWindow::removeAllBreakpoints() {
    for (auto& tab : tabs_) {
        if (tab->kind == Tab::Kind::Text) {
            tab->document->breakpointLines.clear();
        }
    }
    for (const auto& [path, lines] : breakpointStore_) {
        debugger_.setBreakpointLines({}, path);
    }
    breakpointStore_.clear();
    saveSession();
    dirty_ = true;
}

void MainWindow::loadBreakpoints() {
    breakpointStore_.clear();
    nlohmann::json state = Services::shared().folderState(folder_);
    if (!state.is_object() || !state.contains("breakpoints") || !state["breakpoints"].is_array()) {
        return;
    }
    for (const nlohmann::json& entry : state["breakpoints"]) {
        std::string path = entry.value("path", "");
        if (!path.empty() && entry.contains("line") && entry["line"].is_number_unsigned()) {
            breakpointStore_[path].insert(entry["line"].get<size_t>());
        }
    }
}

nlohmann::json MainWindow::breakpointsJson() {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [path, lines] : currentBreakpoints()) {
        for (size_t line : lines) {
            entries.push_back({{"path", path}, {"line", line}});
        }
    }
    return entries;
}

}  // namespace gw::app
