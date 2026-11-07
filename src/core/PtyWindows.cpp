#ifdef _WIN32

#include "Pty.hpp"
#include "WindowsText.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <map>

namespace gw {

namespace {

/// Once this much output is waiting to be shown, reading pauses until it is dealt with.
constexpr size_t kMaxUnacknowledged = 8 * 1024 * 1024;

std::string environmentVariable(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return std::string();
    }
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    value.resize(written);
    return narrowText(value);
}

std::wstring quoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (size_t index = 0;; ++index) {
        size_t backslashes = 0;
        while (index < argument.size() && argument[index] == L'\\') {
            ++index;
            ++backslashes;
        }
        if (index == argument.size()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[index] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(argument[index]);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};

std::wstring environmentBlock(const EnvironmentOverrides& overrides) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables;
    wchar_t* strings = GetEnvironmentStringsW();
    if (strings != nullptr) {
        for (const wchar_t* entry = strings; *entry != L'\0'; entry += wcslen(entry) + 1) {
            std::wstring text = entry;
            size_t equals = text.find(L'=', 1);
            if (equals != std::wstring::npos) {
                variables[text.substr(0, equals)] = text.substr(equals + 1);
            }
        }
        FreeEnvironmentStringsW(strings);
    }
    variables[L"TERM"] = L"xterm-256color";
    variables[L"COLORTERM"] = L"truecolor";
    variables[L"TERM_PROGRAM"] = L"Glasswork";
    variables[L"PATH"] = widenText(searchPathForChildren());
    variables.erase(L"GLASSWORK_TIMING");
    variables.erase(L"GLASSWORK_BENCH");
    for (const auto& [name, value] : overrides) {
        variables[widenText(name)] = widenText(value);
    }
    std::wstring block;
    for (const auto& [name, value] : variables) {
        block += name + L"=" + value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void closeHandle(void*& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(handle));
    }
    handle = nullptr;
}

/// The program's file name without its folder or ".exe".
std::string programName(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".exe") == 0) {
        name.resize(name.size() - 4);
    }
    return name;
}

}  // namespace

std::string defaultShell() {
    // PowerShell 7 when it is installed, else the PowerShell that comes with Windows.
    std::string pwsh = findExecutable("pwsh");
    if (!pwsh.empty()) {
        return pwsh;
    }
    std::string root = environmentVariable(L"SystemRoot");
    std::string windowsPowerShell = (root.empty() ? std::string("C:\\Windows") : root) +
                                    "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    if (GetFileAttributesW(widenText(windowsPowerShell).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return windowsPowerShell;
    }
    std::string comspec = environmentVariable(L"ComSpec");
    return comspec.empty() ? "C:\\Windows\\System32\\cmd.exe" : comspec;
}

Pty::Pty() = default;

Pty::~Pty() {
    stopping_.store(true);
    terminate();
    if (reader_.joinable()) {
        if (reader_.get_id() == std::this_thread::get_id()) {
            reader_.detach();
        } else {
            reader_.join();
        }
    }
    closeHandle(input_);
    closeHandle(output_);
    closeHandle(processHandle_);
    if (console_ != nullptr) {
        ClosePseudoConsole(static_cast<HPCON>(console_));
        console_ = nullptr;
    }
}

bool Pty::start(const PtyOptions& options, std::string* error) {
    if (running_.load()) {
        if (error != nullptr) {
            *error = "already running";
        }
        return false;
    }
    if (reader_.joinable()) {
        reader_.join();
    }

    std::string program = options.program.empty() ? defaultShell() : options.program;
    std::string resolved = findExecutable(program);
    if (resolved.empty()) {
        if (error != nullptr) {
            *error = "could not find \"" + program + "\"";
        }
        return false;
    }
    std::vector<std::string> arguments = options.arguments;
    std::string name = programName(resolved);
    bool isPowerShell = _stricmp(name.c_str(), "pwsh") == 0 || _stricmp(name.c_str(), "powershell") == 0;
    if (options.program.empty() && arguments.empty() && isPowerShell) {
        arguments.push_back("-NoLogo");
    }
    std::wstring commandLine = quoteArgument(widenText(resolved));
    for (const std::string& argument : arguments) {
        commandLine += L" " + quoteArgument(widenText(argument));
    }

    std::string folder = options.workingDirectory;
    if (folder.empty()) {
        folder = environmentVariable(L"USERPROFILE");
    }

    // Two pipes: what we type goes in one, what the console shows comes out of the other.
    HANDLE consoleInput = nullptr, ourInput = nullptr, ourOutput = nullptr, consoleOutput = nullptr;
    if (!CreatePipe(&consoleInput, &ourInput, nullptr, 0) || !CreatePipe(&ourOutput, &consoleOutput, nullptr, 0)) {
        if (error != nullptr) {
            *error = "could not create the terminal's pipes";
        }
        if (consoleInput) CloseHandle(consoleInput);
        if (ourInput) CloseHandle(ourInput);
        if (ourOutput) CloseHandle(ourOutput);
        if (consoleOutput) CloseHandle(consoleOutput);
        return false;
    }
    COORD size = {static_cast<SHORT>(std::max(1, options.columns)), static_cast<SHORT>(std::max(1, options.rows))};
    HPCON console = nullptr;
    HRESULT made = CreatePseudoConsole(size, consoleInput, consoleOutput, 0, &console);
    // The console holds its own copies now.
    CloseHandle(consoleInput);
    CloseHandle(consoleOutput);
    if (FAILED(made)) {
        CloseHandle(ourInput);
        CloseHandle(ourOutput);
        if (error != nullptr) {
            *error = "could not open a terminal (Windows 10 1809 or newer is needed)";
        }
        return false;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<char> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(HPCON), nullptr, nullptr);

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof startup;
    // No standard handles of ours: the program must use the pseudo console, even when this
    // process's own output goes somewhere (a pipe, a log).
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information = {};
    std::wstring environment = environmentBlock(options.environment);
    std::wstring wideFolder = widenText(folder);
    BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                                  wideFolder.empty() ? nullptr : wideFolder.c_str(), &startup.StartupInfo, &information);
    DWORD failure = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        ClosePseudoConsole(console);
        CloseHandle(ourInput);
        CloseHandle(ourOutput);
        if (error != nullptr) {
            *error = "could not start \"" + resolved + "\": " + windowsErrorMessage(failure);
        }
        return false;
    }
    CloseHandle(information.hThread);

    console_ = console;
    processHandle_ = information.hProcess;
    input_ = ourInput;
    output_ = ourOutput;
    processId_ = static_cast<int>(information.dwProcessId);
    master_ = 1;
    shellName_ = name;
    unacknowledged_.store(0);
    stopping_.store(false);
    running_.store(true);
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

void Pty::write(std::string_view bytes) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (input_ == nullptr || !running_.load()) {
        return;
    }
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(input_), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            return;
        }
        bytes.remove_prefix(written);
    }
}

void Pty::resize(int columns, int rows, int pixelWidth, int pixelHeight) {
    (void)pixelWidth;
    (void)pixelHeight;
    if (console_ == nullptr) {
        return;
    }
    COORD size = {static_cast<SHORT>(std::max(1, columns)), static_cast<SHORT>(std::max(1, rows))};
    ResizePseudoConsole(static_cast<HPCON>(console_), size);
}

void Pty::terminate() {
    if (running_.load() && processHandle_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(processHandle_), 1);
    }
}

void Pty::acknowledge(size_t bytes) {
    size_t current = unacknowledged_.load();
    while (true) {
        size_t next = current > bytes ? current - bytes : 0;
        if (unacknowledged_.compare_exchange_weak(current, next)) {
            break;
        }
    }
}

std::string Pty::foregroundProcessName() const {
    // Windows has no foreground process group: the newest program started under the shell is
    // the one in front, else the shell itself.
    if (processId_ <= 0) {
        return std::string();
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return shellName_;
    }
    std::map<DWORD, std::pair<DWORD, std::wstring>> processes;  // id -> parent, name
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        processes[entry.th32ProcessID] = {entry.th32ParentProcessID, entry.szExeFile};
    }
    CloseHandle(snapshot);
    DWORD current = static_cast<DWORD>(processId_);
    std::string name = shellName_;
    for (int depth = 0; depth < 16; ++depth) {
        DWORD child = 0;
        for (const auto& [id, info] : processes) {
            // conhost belongs to the console itself, not to what runs in it.
            if (info.first == current && _wcsicmp(info.second.c_str(), L"conhost.exe") != 0 && id > child) {
                child = id;
            }
        }
        if (child == 0) {
            break;
        }
        current = child;
        name = programName(narrowText(processes[child].second));
    }
    return name;
}

std::string Pty::currentDirectory() const {
    return std::string();  // not readable from outside on Windows; the shell may report it (OSC 7)
}

void Pty::readLoop() {
    HANDLE output = static_cast<HANDLE>(output_);
    HANDLE process = static_cast<HANDLE>(processHandle_);
    // When the program ends, closing the console ends the output pipe, which ends the reading below.
    std::thread closer([this, process] {
        WaitForSingleObject(process, INFINITE);
        if (console_ != nullptr) {
            ClosePseudoConsole(static_cast<HPCON>(console_));
            console_ = nullptr;
        }
    });
    std::vector<char> buffer(64 * 1024);
    while (true) {
        while (unacknowledged_.load() > kMaxUnacknowledged && !stopping_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        DWORD count = 0;
        if (!ReadFile(output, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || count == 0) {
            break;
        }
        if (stopping_.load()) {
            continue;  // drain, so closing the console can't block on a full pipe
        }
        unacknowledged_.fetch_add(count);
        if (onOutput) {
            onOutput(std::string_view(buffer.data(), count));
        }
    }
    if (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT) {
        TerminateProcess(process, 1);
    }
    closer.join();
    DWORD exitCode = 0;
    GetExitCodeProcess(process, &exitCode);
    running_.store(false);
    if (onExit) {
        onExit(static_cast<int>(exitCode));
    }
}

}  // namespace gw

#endif  // _WIN32
