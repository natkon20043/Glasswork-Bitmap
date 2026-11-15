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
    debugger_.onBreakpointStatus = [this] {
        updateBreakpointMarks();
        debugPanel_->reload();
        dirty_ = true;
    };
    debugger_.onBreakpointMoved = [this](const std::string& path, size_t from, size_t to) {
        // Shown where the program will really stop, as the debugger placed it. Two that land on
        // the same line become one.
        Breakpoints breakpoints = breakpointsFor(path);
        auto found = breakpoints.find(from);
        if (found == breakpoints.end()) {
            return;
        }
        BreakpointSettings settings = found->second;
        breakpoints.erase(found);
        breakpoints.emplace(to, settings);
        storeBreakpoints(path, breakpoints);
    };
}

void MainWindow::updateExecutionMarker() {
    Tab* tab = currentTab();
    bool here = tab && tab->kind == Tab::Kind::Text && !tab->path().empty() && executionLine_ >= 0 && !executionPath_.empty() &&
                tabForPath(executionPath_) == tab;
    editor_.setExecutionLine(here ? executionLine_ : -1);
    updateBreakpointMarks();
    dirty_ = true;
}

void MainWindow::updateBreakpointMarks() {
    // While debugging, the breakpoints the debugger couldn't place are drawn hollow.
    std::set<size_t> unverified;
    Tab* tab = currentTab();
    if (tab && tab->kind == Tab::Kind::Text && !tab->path().empty() && debugger_.state() != DebugState::Idle) {
        for (const auto& [line, settings] : tab->document->breakpoints) {
            std::optional<BreakpointStatus> status = debugger_.breakpointStatus(tab->path(), line);
            if (settings.enabled && status && !status->verified) {
                unverified.insert(line);
            }
        }
    }
    editor_.setUnverifiedBreakpoints(std::move(unverified));
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
        if ((language == "c" && extension == ".c") || (language == "cpp" && (extension == ".cpp" || extension == ".cc" || extension == ".cxx")) ||
            (language == "rust" && extension == ".rs") || language == "python") {
            debugFile(tab->path(), language);
            return;
        }
        // JavaScript, TypeScript and other scripts have no debugger here: F5 runs them in the
        // terminal, as Run > Run File does, rather than asking for a program to debug.
        ShellKind kind = shellKindOf(Services::shared().settings().terminalShell);
        if (!commandToRunFile(pathFromUtf8(tab->path()), language, kind).empty()) {
            if (!tab->document->breakpoints.empty()) {
                setStatusMessage("No debugger for " + language + " files here: running it (breakpoints only stop C, C++, Rust and Python)", 8);
            }
            runCurrentFile();
            return;
        }
    }
    debugExecutable();
}

void MainWindow::restartDebugging() {
    // The same program again (built again first when it was built from a file), from the start.
    if (debugger_.state() != DebugState::Idle) {
        debugger_.stop();
    }
    if (relaunch_) {
        std::function<void()> again = relaunch_;
        again();
    } else {
        startDebugging();
    }
}

void MainWindow::debugCurrentFile() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        showError("Nothing to debug", "Save the file first.");
        return;
    }
    debugFile(tab->path(), tab->document->languageId);
}

void MainWindow::debugFile(const std::string& path, const std::string& languageId) {
    if (languageId == "python") {
        debugPython(path);
        return;
    }
    std::string extension = utf8FromPath(pathFromUtf8(path).extension());
    bool isC = languageId == "c" && extension == ".c";
    bool isCpp = languageId == "cpp" && (extension == ".cpp" || extension == ".cc" || extension == ".cxx");
    bool isRust = languageId == "rust" && extension == ".rs";
    if (!isC && !isCpp && !isRust) {
        showError("Can't debug this file", "Glasswork Bitmap can debug Python files, and build a single C, C++ or Rust file (or the "
                                           "Cargo program a Rust file belongs to) for debugging. For other projects, build them first "
                                           "and use Debug > Debug Executable.");
        return;
    }
    std::string source = path;
    std::error_code error;
    fs::path output = fs::temp_directory_path(error) / ("glasswork-debug-" + std::to_string(currentProcessId()));
#ifdef _WIN32
    output += ".exe";
#endif
    std::vector<std::string> command;
    // A Rust file in a Cargo package is built by Cargo, which says (in JSON) where the program went.
    bool cargo = false;
    if (isRust) {
        fs::path manifest = cargoManifestFor(pathFromUtf8(source));
        if (!manifest.empty()) {
            std::string cargoPath = findExecutable("cargo");
            if (cargoPath.empty()) {
                showError("Cargo wasn't found", "Install Rust (with Cargo) and make sure cargo is on the PATH.");
                return;
            }
            cargo = true;
            command = {cargoPath, "build", "--manifest-path", utf8FromPath(manifest), "--message-format=json-render-diagnostics"};
            std::vector<std::string> target = cargoTargetArguments(pathFromUtf8(source), manifest);
            command.insert(command.end(), target.begin(), target.end());
        } else {
            std::string rustc = findExecutable("rustc");
            if (rustc.empty()) {
                showError("No Rust compiler found", "Install Rust and make sure rustc is on the PATH.");
                return;
            }
            command = {rustc, "-g", "-C", "opt-level=0", source, "-o", utf8FromPath(output)};
        }
    } else {
        std::string compiler = isCpp ? (!findExecutable("clang++").empty() ? "clang++" : "g++") : (!findExecutable("clang").empty() ? "clang" : "gcc");
        std::string compilerPath = findExecutable(compiler);
        if (compilerPath.empty()) {
            showError("No compiler found", "Install a C/C++ compiler (MSYS2's mingw-w64 GCC, or LLVM) and make sure it is on the PATH.");
            return;
        }
        command = {compilerPath};
        if (isCpp) {
            command.push_back("-std=c++20");
        }
        command.insert(command.end(), {"-g", "-O0", "-Wall", source, "-o", utf8FromPath(output)});
    }
    saveAllQuietly();
    openSidePanel(SidePanel::Debug);
    debugPanel_->clearConsole();
    debugPanel_->appendConsole("Building " + utf8FromPath(pathFromUtf8(source).filename()) + "...\n", "console");
    std::weak_ptr<bool> alive = alive_;
    std::string built = utf8FromPath(output);
    // Restart builds it again, from the file as it is then.
    std::function<void()> again = [this, source, languageId = std::string(languageId)] { debugFile(source, languageId); };
    runInBackground([this, alive, command, built, cargo, source, again] {
        auto result = std::make_shared<ProcessResult>(runProcess(command));
        runOnMain([this, alive, result, built, cargo, source, again] {
            if (alive.expired()) {
                return;
            }
            // Cargo's standard output is its JSON; the messages meant for people are on standard error.
            std::string messages = (cargo ? std::string() : result->output) + result->errorOutput + result->startError;
            if (!messages.empty()) {
                debugPanel_->appendConsole(messages + (messages.back() == '\n' ? "" : "\n"), result->succeeded() ? "console" : "stderr");
            }
            if (!result->succeeded()) {
                showError("The build failed", "The compiler's messages are in the debugger's console.");
                return;
            }
            std::string program = cargo ? cargoExecutableFor(result->output, pathFromUtf8(source)) : built;
            if (program.empty()) {
                showError("Which program?", "Cargo built several programs and none of them starts in this file. Open the file with "
                                            "its main function, or use Debug > Debug Executable.");
                return;
            }
            launchDebugProgram(program, {});
            relaunch_ = again;
        });
    });
}

void MainWindow::debugPython(const std::string& path) {
    // The program runs in the project's own virtual environment when it has one (its packages),
    // else under python3 from the PATH.
    std::string interpreter;
    if (!folder_.empty()) {
        for (const char* environment : {".venv", "venv", "env"}) {
#ifdef _WIN32
            fs::path candidate = pathFromUtf8(folder_) / environment / "Scripts" / "python.exe";
#else
            fs::path candidate = pathFromUtf8(folder_) / environment / "bin" / "python";
#endif
            std::error_code error;
            if (fs::exists(candidate, error)) {
                interpreter = utf8FromPath(candidate);
                break;
            }
        }
    }
    if (interpreter.empty()) {
        interpreter = findExecutable(pythonProgram());
    }
    if (interpreter.empty()) {
        showError("Python wasn't found", "Install Python 3 and make sure python3 is on the PATH.");
        return;
    }
    // debugpy's adapter needs a Python that has debugpy: the one the editor was installed with
    // (GLASSWORK_DEBUGPY_PYTHON), the program's own, or python3 from the PATH.
    std::vector<std::string> candidates;
    for (const std::string& candidate : {environmentValue("GLASSWORK_DEBUGPY_PYTHON"), interpreter, findExecutable(pythonProgram())}) {
        if (!candidate.empty() && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
            candidates.push_back(candidate);
        }
    }
    std::string directory = folder_.empty() ? utf8FromPath(pathFromUtf8(path).parent_path()) : folder_;
    std::weak_ptr<bool> alive = alive_;
    setStatusMessage("Starting the Python debugger...", 3);
    runInBackground([this, alive, path, interpreter, candidates, directory] {
        std::string adapter;
        for (const std::string& candidate : candidates) {
            if (runProcess({candidate, "-c", "import debugpy"}).succeeded()) {
                adapter = candidate;
                break;
            }
        }
        runOnMain([this, alive, path, interpreter, adapter, directory] {
            if (alive.expired() || debugger_.state() != DebugState::Idle) {
                return;
            }
            if (adapter.empty()) {
                // Without debugpy it can still run, just not stop anywhere.
                setStatusMessage("Running without the debugger: Python debugging needs debugpy (pip install debugpy)", 10);
                Tab* tab = currentTab();
                if (tab && tab->path() == path) {
                    runCurrentFile();
                }
                return;
            }
            saveAllQuietly();
            openSidePanel(SidePanel::Debug);
            debugPanel_->clearConsole();
            debugger_.startPython(path, {}, directory, interpreter, adapter, currentBreakpoints());
            relaunch_ = [this, path] { debugPython(path); };
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
    relaunch_ = [this, program, arguments] {
        debugPanel_->clearConsole();
        launchDebugProgram(program, arguments);
    };
}

// ---------------------------------------------------------------------------
// Breakpoints

std::map<std::string, Breakpoints> MainWindow::currentBreakpoints() {
    for (auto& tab : tabs_) {
        if (tab->kind == Tab::Kind::Text && !tab->path().empty()) {
            if (tab->document->breakpoints.empty()) {
                breakpointStore_.erase(tab->path());
            } else {
                breakpointStore_[tab->path()] = tab->document->breakpoints;
            }
        }
    }
    return breakpointStore_;
}

Breakpoints MainWindow::breakpointsFor(const std::string& path) {
    if (Tab* tab = tabForPath(path); tab && tab->kind == Tab::Kind::Text && tab->document) {
        return tab->document->breakpoints;
    }
    auto found = breakpointStore_.find(path);
    return found != breakpointStore_.end() ? found->second : Breakpoints();
}

void MainWindow::storeBreakpoints(const std::string& path, const Breakpoints& breakpoints) {
    if (Tab* tab = tabForPath(path); tab && tab->kind == Tab::Kind::Text && tab->document) {
        tab->document->breakpoints = breakpoints;
    }
    if (breakpoints.empty()) {
        breakpointStore_.erase(path);
    } else {
        breakpointStore_[path] = breakpoints;
    }
    debugger_.setBreakpoints(path, breakpoints);
    updateBreakpointMarks();
    debugPanel_->reload();
    saveSession();
    dirty_ = true;
}

void MainWindow::breakpointsChanged() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        return;
    }
    storeBreakpoints(tab->path(), tab->document->breakpoints);
}

bool MainWindow::caretLocation(std::string& path, size_t& line) const {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        return false;
    }
    path = tab->path();
    line = tab->document->selection.caret.line;
    return true;
}

void MainWindow::toggleBreakpoint() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        beep();
        return;
    }
    editor_.toggleBreakpointAtLine(tab->document->selection.caret.line);
}

void MainWindow::editBreakpoint(const std::string& path, size_t line, BreakpointField field) {
    Breakpoints breakpoints = breakpointsFor(path);
    auto found = breakpoints.find(line);
    BreakpointSettings settings = found != breakpoints.end() ? found->second : BreakpointSettings();
    DialogOverlay::Options options;
    std::string where = utf8FromPath(pathFromUtf8(path).filename()) + ":" + std::to_string(line + 1);
    std::string* value = nullptr;
    switch (field) {
        case BreakpointField::Condition:
            options.title = "Breakpoint Condition";
            options.lines = {"Stop at " + where + " only when this is true, such as i == 3. Leave it empty to stop every time."};
            options.placeholder = "i == 3";
            value = &settings.condition;
            break;
        case BreakpointField::HitCount:
            options.title = "Breakpoint Hit Count";
            options.lines = {"Stop at " + where + " only once it has been reached this many times, such as 5. Leave it empty to "
                             "stop every time."};
            options.placeholder = "5";
            value = &settings.hitCondition;
            break;
        case BreakpointField::LogMessage:
            options.title = "Logpoint";
            options.lines = {"At " + where + ", print this in the debugger's console and carry on instead of stopping. Put "
                             "expressions in braces: total is {total}. Leave it empty to stop there instead."};
            options.placeholder = "total is {total}";
            value = &settings.logMessage;
            break;
    }
    options.hasField = true;
    options.fieldText = *value;
    options.buttons = {"&OK", "Cancel"};
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, path, line, field, settings](int button, const std::string& text) mutable {
        if (button != 0) {
            return;
        }
        std::string trimmed = text;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
        switch (field) {
            case BreakpointField::Condition: settings.condition = trimmed; break;
            case BreakpointField::HitCount: settings.hitCondition = trimmed; break;
            case BreakpointField::LogMessage: settings.logMessage = trimmed; break;
        }
        settings.enabled = true;
        Breakpoints breakpoints = breakpointsFor(path);
        breakpoints[line] = settings;
        storeBreakpoints(path, breakpoints);
    }));
}

void MainWindow::editBreakpointAtCaret(BreakpointField field) {
    std::string path;
    size_t line = 0;
    if (!caretLocation(path, line)) {
        beep();
        return;
    }
    editBreakpoint(path, line, field);
}

void MainWindow::toggleBreakpointEnabled(const std::string& path, size_t line) {
    Breakpoints breakpoints = breakpointsFor(path);
    auto found = breakpoints.find(line);
    if (found == breakpoints.end()) {
        setStatusMessage("There's no breakpoint on line " + std::to_string(line + 1), 3);
        return;
    }
    found->second.enabled = !found->second.enabled;
    setStatusMessage(found->second.enabled ? "Breakpoint enabled" : "Breakpoint disabled", 2);
    storeBreakpoints(path, breakpoints);
}

void MainWindow::setAllBreakpointsEnabled(bool enabled) {
    for (auto [path, breakpoints] : currentBreakpoints()) {
        for (auto& [line, settings] : breakpoints) {
            settings.enabled = enabled;
        }
        storeBreakpoints(path, breakpoints);
    }
}

void MainWindow::removeAllBreakpoints() {
    for (const auto& [path, breakpoints] : currentBreakpoints()) {
        storeBreakpoints(path, {});
    }
}

void MainWindow::goToBreakpoint(int direction) {
    // Every breakpoint in every file, in order; the next after the caret (or the one before), going
    // round from the last to the first.
    std::vector<std::pair<std::string, size_t>> all;
    for (const auto& [path, breakpoints] : currentBreakpoints()) {
        for (const auto& [line, settings] : breakpoints) {
            all.emplace_back(path, line);
        }
    }
    if (all.empty()) {
        setStatusMessage("There are no breakpoints", 3);
        return;
    }
    std::string path;
    size_t line = 0;
    std::pair<std::string, size_t> here = caretLocation(path, line) ? std::make_pair(path, line) : all.front();
    std::pair<std::string, size_t> target;
    if (direction > 0) {
        auto next = std::upper_bound(all.begin(), all.end(), here);
        target = next != all.end() ? *next : all.front();
    } else {
        auto next = std::lower_bound(all.begin(), all.end(), here);
        target = next != all.begin() ? *std::prev(next) : all.back();
    }
    rememberLocation();
    openPath(target.first, static_cast<long>(target.second) + 1);
}

void MainWindow::runToCursor() {
    std::string path;
    size_t line = 0;
    if (!caretLocation(path, line)) {
        beep();
        return;
    }
    if (debugger_.state() != DebugState::Stopped) {
        setStatusMessage(debugger_.state() == DebugState::Idle ? "Run to Cursor works while debugging: start with F5"
                                                               : "Pause the program first (F6)",
                         4);
        return;
    }
    debugger_.runToLine(path, line);
}

void MainWindow::jumpToCursor() {
    std::string path;
    size_t line = 0;
    if (!caretLocation(path, line)) {
        beep();
        return;
    }
    if (debugger_.state() != DebugState::Stopped) {
        setStatusMessage(debugger_.state() == DebugState::Idle ? "Jump to Cursor works while debugging: start with F5"
                                                               : "Pause the program first (F6)",
                         4);
        return;
    }
    if (!debugger_.canJump()) {
        setStatusMessage(debugger_.adapterName() + " can't jump to another line", 4);
        return;
    }
    debugger_.jumpToLine(path, line);
}

void MainWindow::breakpointMenu(const std::string& path, size_t line, int x, int y) {
    Breakpoints breakpoints = breakpointsFor(path);
    auto found = breakpoints.find(line);
    bool has = found != breakpoints.end();
    Menu menu;
    auto action = [](const std::string& title, std::function<void()> run, std::function<bool()> enabled = nullptr) {
        MenuItem item;
        item.title = title;
        item.run = std::move(run);
        item.enabled = std::move(enabled);
        return item;
    };
    auto toggle = [this, path, line] {
        Breakpoints changed = breakpointsFor(path);
        if (changed.erase(line) == 0) {
            changed[line] = BreakpointSettings();
        }
        storeBreakpoints(path, changed);
    };
    if (has) {
        const BreakpointSettings& settings = found->second;
        menu.items.push_back(action("&Remove Breakpoint", toggle));
        menu.items.push_back(action(settings.condition.empty() ? "Add &Condition..." : "Edit &Condition...",
                                    [this, path, line] { editBreakpoint(path, line, BreakpointField::Condition); }));
        menu.items.push_back(action(settings.hitCondition.empty() ? "Add &Hit Count..." : "Edit &Hit Count...",
                                    [this, path, line] { editBreakpoint(path, line, BreakpointField::HitCount); }));
        menu.items.push_back(action(settings.logMessage.empty() ? "Make a &Logpoint..." : "Edit &Log Message...",
                                    [this, path, line] { editBreakpoint(path, line, BreakpointField::LogMessage); }));
        menu.items.push_back(action(settings.enabled ? "&Disable Breakpoint" : "&Enable Breakpoint",
                                    [this, path, line] { toggleBreakpointEnabled(path, line); }));
    } else {
        menu.items.push_back(action("Add &Breakpoint", toggle));
        menu.items.push_back(action("Add &Conditional Breakpoint...",
                                    [this, path, line] { editBreakpoint(path, line, BreakpointField::Condition); }));
        menu.items.push_back(action("Add &Logpoint...", [this, path, line] { editBreakpoint(path, line, BreakpointField::LogMessage); }));
    }
    menu.items.push_back(MenuItem::separator());
    auto stopped = [this] { return debugger_.state() == DebugState::Stopped; };
    menu.items.push_back(action("R&un to Here", [this, path, line] { debugger_.runToLine(path, line); }, stopped));
    menu.items.push_back(action("&Jump to Here", [this, path, line] {
        if (debugger_.canJump()) {
            debugger_.jumpToLine(path, line);
        } else {
            setStatusMessage(debugger_.adapterName() + " can't jump to another line", 4);
        }
    }, stopped));
    pushOverlay(std::make_unique<MenuOverlay>(menu, x, y, menuHost()));
}

std::vector<DebugPanel::BreakpointRow> MainWindow::breakpointRows() {
    std::vector<DebugPanel::BreakpointRow> rows;
    for (const auto& [path, breakpoints] : currentBreakpoints()) {
        Tab* tab = tabForPath(path);
        for (const auto& [line, settings] : breakpoints) {
            DebugPanel::BreakpointRow row;
            row.path = path;
            row.line = line;
            row.settings = settings;
            std::optional<BreakpointStatus> status = debugger_.breakpointStatus(path, line);
            row.unverified = debugger_.state() != DebugState::Idle && settings.enabled && status && !status->verified;
            if (tab && tab->kind == Tab::Kind::Text && tab->document && line < tab->document->buffer().lineCount()) {
                std::string_view text = tab->document->buffer().line(line);
                size_t start = text.find_first_not_of(" \t");
                row.code = start == std::string_view::npos ? std::string() : std::string(text.substr(start));
            }
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

void MainWindow::loadBreakpoints() {
    breakpointStore_.clear();
    nlohmann::json state = Services::shared().folderState(folder_);
    if (!state.is_object() || !state.contains("breakpoints") || !state["breakpoints"].is_array()) {
        return;
    }
    for (const nlohmann::json& entry : state["breakpoints"]) {
        if (!entry.is_object()) {
            continue;
        }
        std::string path = entry.value("path", "");
        if (path.empty() || !entry.contains("line") || !entry["line"].is_number_unsigned()) {
            continue;
        }
        BreakpointSettings settings;
        settings.enabled = entry.value("enabled", true);
        settings.condition = entry.value("condition", "");
        settings.hitCondition = entry.value("hitCondition", "");
        settings.logMessage = entry.value("logMessage", "");
        breakpointStore_[path][entry["line"].get<size_t>()] = settings;
    }
}

nlohmann::json MainWindow::breakpointsJson() {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& [path, breakpoints] : currentBreakpoints()) {
        for (const auto& [line, settings] : breakpoints) {
            nlohmann::json entry = {{"path", path}, {"line", line}};
            if (!settings.enabled) {
                entry["enabled"] = false;
            }
            if (!settings.condition.empty()) {
                entry["condition"] = settings.condition;
            }
            if (!settings.hitCondition.empty()) {
                entry["hitCondition"] = settings.hitCondition;
            }
            if (!settings.logMessage.empty()) {
                entry["logMessage"] = settings.logMessage;
            }
            entries.push_back(entry);
        }
    }
    return entries;
}

}  // namespace gw::app
