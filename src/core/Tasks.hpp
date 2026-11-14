#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gw {

/// A build, test run or script the project can be asked to do.
struct Task {
    // Build file or script detected -> what the task does.
    enum class Kind { Build, Run, Test, Other };

    /// "Build (CMake)"
    std::string label;
    /// Typed into the terminal and run from the project folder.
    std::string command;
    Kind kind = Kind::Other;
};

// The shell a task is typed into -> how its commands are joined and quoted.
enum class ShellKind { Posix, PowerShell, Cmd };

/// PowerShell on Windows, a POSIX shell elsewhere.
ShellKind defaultShellKind();

// ---- Detecting

/// Reads CMake, Swift packages, Cargo, Go modules, dotnet, npm scripts and Makefiles. Builds come first.
std::vector<Task> detectTasks(const std::filesystem::path& folder, ShellKind shell = defaultShellKind());

/// Runs one file alone (a script, or a single C/C++/Rust file compiled first; a Rust file inside a
/// Cargo package runs with `cargo run`). Empty for files that can't.
std::string commandToRunFile(const std::filesystem::path& file, const std::string& languageId, ShellKind shell = defaultShellKind());

/// The Cargo.toml of the nearest folder above a Rust file that has one; empty when the file is in no
/// Cargo package.
std::filesystem::path cargoManifestFor(const std::filesystem::path& file);
/// The `cargo run` / `cargo build` arguments choosing the program a file is the root of, as separate
/// words: {"--bin", "name"} for src/bin/name.rs and src/bin/name/main.rs (and src/main.rs, named
/// after the package), {"--example", "name"} for
/// examples/name.rs and examples/name/main.rs; none for anything else (Cargo's default program).
std::vector<std::string> cargoTargetArguments(const std::filesystem::path& file, const std::filesystem::path& manifest);
/// From `cargo build --message-format=json...` output: the program built from `file` (the target
/// whose root it is), else the only program built. Empty when there is none, or several and none is
/// the file's.
std::string cargoExecutableFor(const std::string& messages, const std::filesystem::path& file);

// ---- Shell

/// From the shell program's name ("pwsh.exe", "cmd", "/bin/zsh").
ShellKind shellKindFor(const std::string& shellProgram);
/// Single-quotes a string for a POSIX shell.
std::string shellQuote(const std::string& text);
std::string quoteForShell(const std::string& text, ShellKind shell);
/// Runs `second` only if `first` succeeded.
std::string thenCommand(const std::string& first, const std::string& second, ShellKind shell);
/// "python3" on Linux; "python" or "py" on Windows.
std::string pythonProgram();

}  // namespace gw
