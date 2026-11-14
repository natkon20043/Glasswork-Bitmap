#include "Tasks.hpp"

#include "Jsonc.hpp"
#include "Paths.hpp"
#include "Process.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace gw {

namespace {

bool pathExists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::stringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

bool hasFileWithExtension(const std::filesystem::path& folder, const std::string& extension) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() == extension) {
            return true;
        }
    }
    return false;
}

void add(std::vector<Task>& tasks, const std::string& label, const std::string& command, Task::Kind kind) {
    tasks.push_back(Task{label, command, kind});
}

/// npm scripts from package.json, using whichever package manager the project uses.
void addNodeTasks(const std::filesystem::path& folder, std::vector<Task>& tasks, ShellKind shell) {
    std::filesystem::path manifest = folder / "package.json";
    if (!pathExists(manifest)) {
        return;
    }
    std::string runner = "npm run";
    if (pathExists(folder / "pnpm-lock.yaml")) {
        runner = "pnpm run";
    } else if (pathExists(folder / "yarn.lock")) {
        runner = "yarn run";
    } else if (pathExists(folder / "bun.lockb") || pathExists(folder / "bun.lock")) {
        runner = "bun run";
    }

    nlohmann::json document = parseJsonc(readFile(manifest));
    if (document.is_discarded() || !document.is_object() || !document.contains("scripts") || !document["scripts"].is_object()) {
        return;
    }
    for (const auto& [name, script] : document["scripts"].items()) {
        if (!script.is_string()) {
            continue;
        }
        Task::Kind kind = Task::Kind::Other;
        if (name == "build" || name.rfind("build:", 0) == 0) {
            kind = Task::Kind::Build;
        } else if (name == "start" || name == "dev" || name == "serve") {
            kind = Task::Kind::Run;
        } else if (name == "test" || name.rfind("test:", 0) == 0) {
            kind = Task::Kind::Test;
        }
        // Quoted: a script name is only a name, whatever characters it has.
        add(tasks, "npm script: " + name, runner + " " + quoteForShell(name, shell), kind);
    }
}

/// Targets of a Makefile that look like real targets (not patterns or variables).
void addMakeTasks(const std::filesystem::path& folder, std::vector<Task>& tasks) {
    std::filesystem::path makefile;
    for (const char* name : {"Makefile", "makefile", "GNUmakefile"}) {
        if (pathExists(folder / name)) {
            makefile = folder / name;
            break;
        }
    }
    if (makefile.empty()) {
        return;
    }
    // MSYS2 and MinGW name it mingw32-make.
    std::string make = findExecutable("make").empty() && !findExecutable("mingw32-make").empty() ? "mingw32-make" : "make";
    add(tasks, "Build (make)", make, Task::Kind::Build);
    std::istringstream input(readFile(makefile));
    std::string line;
    std::regex target(R"(^([A-Za-z][A-Za-z0-9_.-]*)\s*:(?!=))");
    size_t added = 0;
    while (std::getline(input, line) && added < 15) {
        std::smatch match;
        if (std::regex_search(line, match, target)) {
            std::string name = match[1];
            if (name == "all" || name == ".PHONY" || name == "default") {
                continue;
            }
            Task::Kind kind = Task::Kind::Other;
            if (name == "test" || name == "check") {
                kind = Task::Kind::Test;
            } else if (name == "run") {
                kind = Task::Kind::Run;
            }
            add(tasks, make + " " + name, make + " " + name, kind);
            ++added;
        }
    }
}

}  // namespace

std::string shellQuote(const std::string& text) {
    std::string quoted = "'";
    for (char character : text) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    return quoted + "'";
}

ShellKind shellKindFor(const std::string& shellProgram) {
    std::string name = shellProgram;
    size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) {
        name.resize(name.size() - 4);
    }
    if (name == "pwsh" || name == "powershell") {
        return ShellKind::PowerShell;
    }
    if (name == "cmd") {
        return ShellKind::Cmd;
    }
    if (name.empty()) {
        return defaultShellKind();
    }
    return ShellKind::Posix;
}

ShellKind defaultShellKind() {
#ifdef _WIN32
    return ShellKind::PowerShell;
#else
    return ShellKind::Posix;
#endif
}

std::string quoteForShell(const std::string& text, ShellKind shell) {
    switch (shell) {
        case ShellKind::Posix:
            return shellQuote(text);
        case ShellKind::PowerShell: {
            std::string quoted = "'";
            for (char character : text) {
                quoted += character;
                if (character == '\'') {
                    quoted += '\'';  // doubled inside single quotes
                }
            }
            return quoted + "'";
        }
        case ShellKind::Cmd:
            if (text.find_first_of(" \t&|<>^()\"") == std::string::npos) {
                return text;
            }
            return "\"" + text + "\"";
    }
    return text;
}

std::string thenCommand(const std::string& first, const std::string& second, ShellKind shell) {
    if (shell == ShellKind::PowerShell) {
        // Windows PowerShell 5 has no "&&"; this works in every version.
        return first + "; if ($?) { " + second + " }";
    }
    return first + " && " + second;
}

std::string runProgramCommand(const std::string& quotedProgram, ShellKind shell) {
    return shell == ShellKind::PowerShell ? "& " + quotedProgram : quotedProgram;
}

std::vector<Task> detectTasks(const std::filesystem::path& folder, ShellKind shell) {
    std::vector<Task> tasks;
    if (pathExists(folder / "CMakeLists.txt")) {
        std::string configure = "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release";
        if (!findExecutable("ninja").empty()) {
            configure = "cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release";
        }
        add(tasks, "Build (CMake)", thenCommand(configure, "cmake --build build", shell), Task::Kind::Build);
        add(tasks, "Build again (CMake)", "cmake --build build", Task::Kind::Build);
        add(tasks, "Test (CTest)", "ctest --test-dir build --output-on-failure", Task::Kind::Test);
    }
    if (pathExists(folder / "Package.swift")) {
        add(tasks, "Build (Swift)", "swift build", Task::Kind::Build);
        add(tasks, "Run (Swift)", "swift run", Task::Kind::Run);
        add(tasks, "Test (Swift)", "swift test", Task::Kind::Test);
    }
    if (pathExists(folder / "Cargo.toml")) {
        add(tasks, "Build (Cargo)", "cargo build", Task::Kind::Build);
        add(tasks, "Check (Cargo)", "cargo check", Task::Kind::Build);
        add(tasks, "Run (Cargo)", "cargo run", Task::Kind::Run);
        add(tasks, "Test (Cargo)", "cargo test", Task::Kind::Test);
    }
    if (pathExists(folder / "go.mod")) {
        add(tasks, "Build (Go)", "go build ./...", Task::Kind::Build);
        add(tasks, "Run (Go)", "go run .", Task::Kind::Run);
        add(tasks, "Test (Go)", "go test ./...", Task::Kind::Test);
    }
    if (hasFileWithExtension(folder, ".sln") || hasFileWithExtension(folder, ".csproj")) {
        add(tasks, "Build (dotnet)", "dotnet build", Task::Kind::Build);
        add(tasks, "Run (dotnet)", "dotnet run", Task::Kind::Run);
        add(tasks, "Test (dotnet)", "dotnet test", Task::Kind::Test);
    }
    if (pathExists(folder / "pyproject.toml") || pathExists(folder / "pytest.ini")) {
        add(tasks, "Test (pytest)", pythonProgram() + " -m pytest", Task::Kind::Test);
    }
    addNodeTasks(folder, tasks, shell);
    addMakeTasks(folder, tasks);

    // Builds first, then runs, tests and the rest, each group in the order found.
    std::stable_sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
    return tasks;
}

std::string pythonProgram() {
#ifdef _WIN32
    if (!findExecutable("python").empty() && findExecutable("python").find("WindowsApps") == std::string::npos) {
        return "python";
    }
    if (!findExecutable("py").empty()) {
        return "py";
    }
    return "python";
#else
    return "python3";
#endif
}

std::string cCompiler(bool cplusplus) {
    if (cplusplus) {
        if (!findExecutable("clang++").empty()) {
            return "clang++";
        }
        return "g++";
    }
    if (!findExecutable("clang").empty()) {
        return "clang";
    }
    return "gcc";
}

std::filesystem::path cargoManifestFor(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::path folder = std::filesystem::absolute(file, error).parent_path();
    while (!folder.empty()) {
        if (pathExists(folder / "Cargo.toml")) {
            return folder / "Cargo.toml";
        }
        if (!folder.has_relative_path() || folder == folder.parent_path()) {
            break;
        }
        folder = folder.parent_path();
    }
    return std::filesystem::path();
}

namespace {

/// The `name` in a Cargo.toml's [package] table; empty when there is none.
std::string cargoPackageName(const std::filesystem::path& manifest) {
    std::istringstream lines(readFile(manifest));
    std::string line;
    bool inPackage = false;
    static const std::regex table(R"(^\s*\[\[?\s*([^\]]*?)\s*\]\]?\s*(#.*)?$)");  // [table] or [[array]]
    static const std::regex name(R"(^\s*name\s*=\s*["']([^"']+)["']\s*(#.*)?$)");
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::smatch match;
        if (std::regex_match(line, match, table)) {
            inPackage = match[1] == "package";
        } else if (inPackage && std::regex_match(line, match, name)) {
            return match[1];
        }
    }
    return std::string();
}

}  // namespace

std::vector<std::string> cargoTargetArguments(const std::filesystem::path& file, const std::filesystem::path& manifest) {
    std::error_code error;
    std::filesystem::path relative = std::filesystem::absolute(file, error).lexically_relative(manifest.parent_path());
    std::vector<std::string> parts;
    for (const auto& part : relative) {
        parts.push_back(utf8FromPath(part));
    }
    auto stem = [&](const std::string& name) { return utf8FromPath(pathFromUtf8(name).stem()); };
    bool rust = file.extension() == ".rs";
    if (rust && parts.size() == 2 && parts[0] == "src" && parts[1] == "main.rs") {
        // The package's own program, named after the package: chosen by name, since a package
        // with programs in src/bin too has no single default to run.
        std::string package = cargoPackageName(manifest);
        if (!package.empty()) {
            return {"--bin", package};
        }
        return {};
    }
    if (rust && parts.size() == 3 && parts[0] == "src" && parts[1] == "bin") {
        return {"--bin", stem(parts[2])};
    }
    if (rust && parts.size() == 4 && parts[0] == "src" && parts[1] == "bin" && parts[3] == "main.rs") {
        return {"--bin", parts[2]};
    }
    if (rust && parts.size() == 2 && parts[0] == "examples") {
        return {"--example", stem(parts[1])};
    }
    if (rust && parts.size() == 3 && parts[0] == "examples" && parts[2] == "main.rs") {
        return {"--example", parts[1]};
    }
    return {};
}

std::string cargoExecutableFor(const std::string& messages, const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::path wanted = std::filesystem::weakly_canonical(file, error);
    std::vector<std::string> programs;
    std::istringstream lines(messages);
    std::string line;
    while (std::getline(lines, line)) {
        nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (!message.is_object() || message.value("reason", "") != "compiler-artifact" || !message.contains("executable") ||
            !message["executable"].is_string()) {
            continue;
        }
        std::string program = message["executable"].get<std::string>();
        if (message.contains("target") && message["target"].is_object() && message["target"].contains("src_path") &&
            message["target"]["src_path"].is_string()) {
            std::filesystem::path root = std::filesystem::weakly_canonical(pathFromUtf8(message["target"]["src_path"].get<std::string>()), error);
            if (!wanted.empty() && root == wanted) {
                return program;
            }
        }
        if (std::find(programs.begin(), programs.end(), program) == programs.end()) {
            programs.push_back(program);
        }
    }
    return programs.size() == 1 ? programs.front() : std::string();
}

std::string commandToRunFile(const std::filesystem::path& file, const std::string& languageId, ShellKind shell) {
    std::string path = quoteForShell(utf8FromPath(file), shell);
    // Compiled programs go in the user's own temporary folder, not a shared one, where another
    // account could put its own file in their place.
    std::string program;
    switch (shell) {
        case ShellKind::Posix:
            program = "\"${TMPDIR:-/tmp}/glasswork-run\"";
            break;
        case ShellKind::PowerShell:
            program = "\"$env:TEMP\\glasswork-run.exe\"";
            break;
        case ShellKind::Cmd:
            program = "\"%TEMP%\\glasswork-run.exe\"";
            break;
    }
    auto compileThenRun = [&](const std::string& compile) {
        return thenCommand(compile + " -o " + program, runProgramCommand(program, shell), shell);
    };
    std::string extension = file.extension().string();
    if (languageId == "python") {
        return pythonProgram() + " " + path;
    }
    if (languageId == "javascript" || languageId == "javascriptreact") {
        return "node " + path;
    }
    if (languageId == "typescript" || languageId == "typescriptreact") {
        // tsx when it is installed; otherwise npx fetches it.
        return (findExecutable("tsx").empty() ? "npx tsx " : "tsx ") + path;
    }
    if (languageId == "swift") {
        return "swift " + path;
    }
    if (languageId == "shellscript") {
        return "bash " + path;
    }
    if (languageId == "powershell") {
        return shell == ShellKind::PowerShell ? "& " + path : "powershell -NoProfile -File " + path;
    }
    if (languageId == "bat") {
        return shell == ShellKind::PowerShell ? "& " + path : path;
    }
    if (languageId == "ruby") {
        return "ruby " + path;
    }
    if (languageId == "lua") {
        return "lua " + path;
    }
    if (languageId == "perl") {
        return "perl " + path;
    }
    if (languageId == "php") {
        return "php " + path;
    }
    if (languageId == "go") {
        return "go run " + path;
    }
    if (languageId == "rust") {
        // A file in a Cargo package needs Cargo (its crates and modules); a file alone, just rustc.
        std::filesystem::path manifest = cargoManifestFor(file);
        if (!manifest.empty()) {
            std::string command = "cargo run --manifest-path " + quoteForShell(utf8FromPath(manifest), shell);
            for (const std::string& argument : cargoTargetArguments(file, manifest)) {
                command += " " + (argument.rfind("--", 0) == 0 ? argument : quoteForShell(argument, shell));
            }
            return command;
        }
        return compileThenRun("rustc " + path);
    }
    if (languageId == "cpp" && (extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".mm")) {
        return compileThenRun(cCompiler(true) + " -std=c++20 -Wall " + path);
    }
    if (languageId == "c" && extension == ".c") {
        return compileThenRun(cCompiler(false) + " -Wall " + path);
    }
    return std::string();
}

}  // namespace gw
