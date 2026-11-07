#ifdef _WIN32

#include "Process.hpp"
#include "WindowsText.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>

namespace gw {

namespace {

// ---------------------------------------------------------------------------
// Small helpers

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

bool isFile(const std::string& path) {
    DWORD attributes = GetFileAttributesW(widenText(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t separator = path.find(';', start);
        if (separator == std::string::npos) {
            separator = path.size();
        }
        if (separator > start) {
            std::string part = path.substr(start, separator - start);
            if (part.size() >= 2 && part.front() == '"' && part.back() == '"') {
                part = part.substr(1, part.size() - 2);
            }
            parts.push_back(part);
        }
        start = separator + 1;
    }
    return parts;
}

/// Where tools commonly live on Windows even when the PATH a GUI app inherits doesn't list them.
std::vector<std::string> extraSearchFolders() {
    std::vector<std::string> folders;
    std::string appData = environmentVariable(L"APPDATA");
    std::string localAppData = environmentVariable(L"LOCALAPPDATA");
    std::string profile = environmentVariable(L"USERPROFILE");
    std::string programFiles = environmentVariable(L"ProgramFiles");
    if (!appData.empty()) {
        folders.push_back(appData + "\\npm");
    }
    if (!profile.empty()) {
        folders.push_back(profile + "\\.cargo\\bin");
        folders.push_back(profile + "\\go\\bin");
        folders.push_back(profile + "\\scoop\\shims");
        folders.push_back(profile + "\\.local\\bin");
    }
    if (!localAppData.empty()) {
        folders.push_back(localAppData + "\\Microsoft\\WinGet\\Links");
        folders.push_back(localAppData + "\\Programs\\Python\\Launcher");
    }
    if (!programFiles.empty()) {
        folders.push_back(programFiles + "\\nodejs");
        folders.push_back(programFiles + "\\Git\\cmd");
        folders.push_back(programFiles + "\\LLVM\\bin");
        folders.push_back(programFiles + "\\Go\\bin");
    }
    folders.push_back("C:\\msys64\\ucrt64\\bin");
    folders.push_back("C:\\msys64\\clang64\\bin");
    folders.push_back("C:\\msys64\\mingw64\\bin");
    std::string nvm = newestNvmBinFolder();
    if (!nvm.empty()) {
        folders.push_back(nvm);
    }
    return folders;
}

/// The extensions Windows runs programs with, most likely first.
std::vector<std::string> executableExtensions() {
    std::vector<std::string> extensions = {".exe", ".cmd", ".bat", ".com"};
    for (const std::string& extension : [] {
             std::vector<std::string> parts;
             std::string text = environmentVariable(L"PATHEXT");
             size_t start = 0;
             while (start < text.size()) {
                 size_t end = text.find(';', start);
                 if (end == std::string::npos) {
                     end = text.size();
                 }
                 if (end > start) {
                     parts.push_back(text.substr(start, end - start));
                 }
                 start = end + 1;
             }
             return parts;
         }()) {
        std::string lower = lowerCase(extension);
        if (std::find(extensions.begin(), extensions.end(), lower) == extensions.end()) {
            extensions.push_back(lower);
        }
    }
    return extensions;
}

bool hasExecutableExtension(const std::string& name) {
    std::string lower = lowerCase(name);
    for (const std::string& extension : executableExtensions()) {
        if (lower.size() > extension.size() && lower.compare(lower.size() - extension.size(), extension.size(), extension) == 0) {
            return true;
        }
    }
    return false;
}

bool isBatchFile(const std::string& path) {
    std::string lower = lowerCase(path);
    auto endsWith = [&lower](const char* suffix) {
        size_t length = std::char_traits<char>::length(suffix);
        return lower.size() >= length && lower.compare(lower.size() - length, length, suffix) == 0;
    };
    return endsWith(".cmd") || endsWith(".bat");
}

/// One argument quoted the way the C runtime's command-line parser takes it apart again.
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

/// cmd.exe's own escapes, for the arguments of a batch file (npm's program shims are batch files).
std::wstring quoteForCmd(const std::wstring& argument) {
    std::wstring quoted = quoteArgument(argument);
    std::wstring escaped;
    for (wchar_t character : quoted) {
        if (character == L'^' || character == L'&' || character == L'|' || character == L'<' || character == L'>' ||
            character == L'%' || character == L'!' || character == L'(' || character == L')') {
            escaped.push_back(L'^');
        }
        escaped.push_back(character);
    }
    return escaped;
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

/// The environment block the child gets: ours, with a fuller PATH, then the caller's overrides.
std::wstring buildEnvironmentBlock(const EnvironmentOverrides& overrides) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables;
    wchar_t* strings = GetEnvironmentStringsW();
    if (strings != nullptr) {
        for (const wchar_t* entry = strings; *entry != L'\0'; entry += wcslen(entry) + 1) {
            std::wstring text = entry;
            size_t equals = text.find(L'=', 1);  // "=C:=C:\\" style entries start with '='
            if (equals == std::wstring::npos) {
                continue;
            }
            variables[text.substr(0, equals)] = text.substr(equals + 1);
        }
        FreeEnvironmentStringsW(strings);
    }
    variables[L"PATH"] = widenText(searchPathForChildren());
    for (const auto& [name, value] : overrides) {
        if (value.empty()) {
            variables.erase(widenText(name));
        } else {
            variables[widenText(name)] = widenText(value);
        }
    }
    std::wstring block;
    for (const auto& [name, value] : variables) {
        block += name;
        block += L'=';
        block += value;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

/// A job that ends every process in it when its last handle closes, so stopping a program also
/// stops whatever it started.
HANDLE makeKillOnCloseJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    return job;
}

void closeHandle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

struct Pipes {
    HANDLE childInput = nullptr;   // read end, the child's standard input
    HANDLE input = nullptr;        // our write end
    HANDLE output = nullptr;       // our read end of the child's standard output
    HANDLE childOutput = nullptr;
    HANDLE error = nullptr;
    HANDLE childError = nullptr;

    void closeAll() {
        closeHandle(childInput);
        closeHandle(input);
        closeHandle(output);
        closeHandle(childOutput);
        closeHandle(error);
        closeHandle(childError);
    }
};

bool makePipes(Pipes& pipes) {
    SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&pipes.childInput, &pipes.input, &inheritable, 0) ||
        !CreatePipe(&pipes.output, &pipes.childOutput, &inheritable, 0) ||
        !CreatePipe(&pipes.error, &pipes.childError, &inheritable, 0)) {
        pipes.closeAll();
        return false;
    }
    // Our ends stay ours.
    SetHandleInformation(pipes.input, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(pipes.output, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(pipes.error, HANDLE_FLAG_INHERIT, 0);
    return true;
}

/// Starts the program with its standard streams on the pipes' child ends, inside `job`. Only
/// those three handles are inherited, so programs started at the same time on other threads
/// can't end up holding each other's pipes open.
bool spawnProgram(const std::vector<std::string>& argv, const ProcessOptions& options, Pipes& pipes, HANDLE job,
                  PROCESS_INFORMATION& information, std::string& error) {
    if (argv.empty()) {
        error = "no program to run";
        return false;
    }
    std::string program = findExecutable(argv[0]);
    if (program.empty()) {
        error = "could not find \"" + argv[0] + "\"";
        return false;
    }

    std::wstring commandLine;
    std::wstring application;
    if (isBatchFile(program)) {
        // Batch files run through cmd.exe; /s keeps the outer quotes from being reinterpreted.
        std::string comspec = environmentVariable(L"ComSpec");
        application = widenText(comspec.empty() ? "C:\\Windows\\System32\\cmd.exe" : comspec);
        std::wstring inner = quoteForCmd(widenText(program));
        for (size_t index = 1; index < argv.size(); ++index) {
            inner += L' ';
            inner += quoteForCmd(widenText(argv[index]));
        }
        commandLine = L"cmd.exe /d /s /c \"" + inner + L"\"";
    } else {
        application = widenText(program);
        commandLine = quoteArgument(widenText(program));
        for (size_t index = 1; index < argv.size(); ++index) {
            commandLine += L' ';
            commandLine += quoteArgument(widenText(argv[index]));
        }
    }
    std::wstring environment = buildEnvironmentBlock(options.environment);
    std::wstring folder = widenText(options.workingDirectory);

    HANDLE inherited[3] = {pipes.childInput, pipes.childOutput, pipes.childError};
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<char> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
        error = "could not prepare the program's start";
        return false;
    }
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr);

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = pipes.childInput;
    startup.StartupInfo.hStdOutput = pipes.childOutput;
    startup.StartupInfo.hStdError = pipes.childError;
    startup.lpAttributeList = attributes;

    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED;
    BOOL created = CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags,
                                  environment.data(), folder.empty() ? nullptr : folder.c_str(), &startup.StartupInfo,
                                  &information);
    DWORD failure = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) {
        error = "could not start \"" + program + "\": " + windowsErrorMessage(failure);
        return false;
    }
    if (job != nullptr) {
        AssignProcessToJobObject(job, information.hProcess);
    }
    ResumeThread(information.hThread);
    closeHandle(information.hThread);
    return true;
}

/// Reads a pipe until it closes, appending at most `limit` bytes.
void readAll(HANDLE pipe, std::string& into, size_t limit) {
    char chunk[64 * 1024];
    while (true) {
        DWORD count = 0;
        if (!ReadFile(pipe, chunk, sizeof chunk, &count, nullptr) || count == 0) {
            return;
        }
        size_t room = limit > into.size() ? limit - into.size() : 0;
        into.append(chunk, std::min<size_t>(count, room));
    }
}

bool writeAll(HANDLE pipe, std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD written = 0;
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1 << 20));
        if (!WriteFile(pipe, bytes.data(), chunk, &written, nullptr)) {
            return false;
        }
        bytes.remove_prefix(written);
    }
    return true;
}

}  // namespace

int exitCodeFromStatus(int status) {
    return status;
}

// ---------------------------------------------------------------------------
// Finding programs

std::string searchPathForChildren() {
    std::string path = environmentVariable(L"PATH");
    std::vector<std::string> present = splitPath(path);
    for (const std::string& folder : extraSearchFolders()) {
        bool alreadyThere = false;
        for (const std::string& known : present) {
            if (_stricmp(known.c_str(), folder.c_str()) == 0) {
                alreadyThere = true;
                break;
            }
        }
        DWORD attributes = GetFileAttributesW(widenText(folder).c_str());
        bool exists = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!alreadyThere && exists) {
            if (!path.empty() && path.back() != ';') {
                path += ";";
            }
            path += folder;
            present.push_back(folder);
        }
    }
    return path;
}

long versionNumber(const std::string& text) {
    long parts[3] = {0, 0, 0};
    int index = 0;
    long current = 0;
    bool any = false;
    for (char character : text) {
        if (character >= '0' && character <= '9') {
            current = current * 10 + (character - '0');
            any = true;
        } else if (character == '.' && index < 2) {
            parts[index++] = current;
            current = 0;
        } else if (character == 'v' && !any) {
            continue;
        } else {
            break;
        }
    }
    parts[index] = current;
    return any ? parts[0] * 1000000 + parts[1] * 1000 + parts[2] : 0;
}

std::string newestNvmBinFolder() {
    // nvm for Windows keeps the chosen Node behind one link folder.
    std::string link = environmentVariable(L"NVM_SYMLINK");
    if (!link.empty() && isFile(link + "\\node.exe")) {
        return link;
    }
    return std::string();
}

std::string findExecutable(const std::string& name) {
    if (name.empty()) {
        return std::string();
    }
    std::vector<std::string> extensions = executableExtensions();
    auto tryCandidate = [&](const std::string& candidate) -> std::string {
        if (hasExecutableExtension(candidate) && isFile(candidate)) {
            return candidate;
        }
        for (const std::string& extension : extensions) {
            if (isFile(candidate + extension)) {
                return candidate + extension;
            }
        }
        return std::string();
    };
    bool hasFolder = name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                     (name.size() >= 2 && name[1] == ':');
    if (hasFolder) {
        std::string candidate = name;
        std::replace(candidate.begin(), candidate.end(), '/', '\\');
        return tryCandidate(candidate);
    }
    for (const std::string& folder : splitPath(searchPathForChildren())) {
        std::string found = tryCandidate(folder + "\\" + name);
        if (!found.empty()) {
            return found;
        }
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// runProcess: run to completion

ProcessResult runProcess(const std::vector<std::string>& argv, const ProcessOptions& options) {
    ProcessResult result;
    Pipes pipes;
    if (!makePipes(pipes)) {
        result.startError = "could not create pipes";
        return result;
    }
    HANDLE job = makeKillOnCloseJob();
    PROCESS_INFORMATION information = {};
    std::string spawnError;
    bool started = spawnProgram(argv, options, pipes, job, information, spawnError);
    closeHandle(pipes.childInput);
    closeHandle(pipes.childOutput);
    closeHandle(pipes.childError);
    if (!started) {
        result.startError = spawnError;
        pipes.closeAll();
        closeHandle(job);
        return result;
    }
    result.started = true;

    std::thread writer;
    if (options.input.empty()) {
        closeHandle(pipes.input);
    } else {
        HANDLE input = pipes.input;
        pipes.input = nullptr;
        std::string text = options.input;
        writer = std::thread([input, text] {
            writeAll(input, text);
            CloseHandle(input);
        });
    }
    std::thread errorReader([&] { readAll(pipes.error, result.errorOutput, options.maxOutputBytes); });
    std::thread outputReader([&] { readAll(pipes.output, result.output, options.maxOutputBytes); });

    DWORD wait = options.timeoutSeconds > 0 ? static_cast<DWORD>(options.timeoutSeconds * 1000) : INFINITE;
    if (WaitForSingleObject(information.hProcess, wait) == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateJobObject(job, 1);
        WaitForSingleObject(information.hProcess, 5000);
    } else {
        // Gone: give what it started a moment to finish writing, then end it, so pipes a
        // leftover helper holds open can't keep us waiting.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD available = 0;
            bool outOpen = PeekNamedPipe(pipes.output, nullptr, 0, nullptr, &available, nullptr) != 0;
            bool errOpen = PeekNamedPipe(pipes.error, nullptr, 0, nullptr, &available, nullptr) != 0;
            if (!outOpen && !errOpen) {
                break;
            }
            Sleep(5);
        }
        TerminateJobObject(job, 0);
    }
    outputReader.join();
    errorReader.join();
    if (writer.joinable()) {
        writer.join();
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(information.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    closeHandle(information.hProcess);
    pipes.closeAll();
    closeHandle(job);
    return result;
}

// ---------------------------------------------------------------------------
// ChildProcess: a live conversation

ChildProcess::ChildProcess() = default;

ChildProcess::~ChildProcess() {
    terminate();
    if (reader_.joinable()) {
        if (reader_.get_id() == std::this_thread::get_id()) {
            reader_.detach();
        } else {
            reader_.join();
        }
    }
    closeInput();
    HANDLE process = processHandle_;
    HANDLE job = job_;
    closeHandle(process);
    closeHandle(job);
    processHandle_ = nullptr;
    job_ = nullptr;
}

bool ChildProcess::start(const std::vector<std::string>& argv, const ProcessOptions& options, std::string* error) {
    if (running_.load()) {
        if (error != nullptr) {
            *error = "already running";
        }
        return false;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    // A previous run's leftovers.
    HANDLE oldProcess = processHandle_;
    HANDLE oldJob = job_;
    closeHandle(oldProcess);
    closeHandle(oldJob);
    processHandle_ = nullptr;
    job_ = nullptr;

    Pipes pipes;
    if (!makePipes(pipes)) {
        if (error != nullptr) {
            *error = "could not create pipes";
        }
        return false;
    }
    HANDLE job = makeKillOnCloseJob();
    PROCESS_INFORMATION information = {};
    std::string spawnError;
    bool started = spawnProgram(argv, options, pipes, job, information, spawnError);
    closeHandle(pipes.childInput);
    closeHandle(pipes.childOutput);
    closeHandle(pipes.childError);
    if (!started) {
        if (error != nullptr) {
            *error = spawnError;
        }
        pipes.closeAll();
        closeHandle(job);
        return false;
    }

    processHandle_ = information.hProcess;
    job_ = job;
    inputHandle_ = pipes.input;
    outputHandle_ = pipes.output;
    errorHandle_ = pipes.error;
    processId_ = static_cast<int>(information.dwProcessId);
    inputDescriptor_ = 1;
    outputDescriptor_ = 1;
    errorDescriptor_ = 1;
    if (!options.input.empty()) {
        writeAll(pipes.input, options.input);
    }
    running_.store(true);
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

bool ChildProcess::write(std::string_view bytes) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (inputHandle_ == nullptr || !running_.load()) {
        return false;
    }
    return writeAll(inputHandle_, bytes);
}

void ChildProcess::closeInput() {
    std::lock_guard<std::mutex> lock(writeMutex_);
    HANDLE input = inputHandle_;
    closeHandle(input);
    inputHandle_ = nullptr;
    inputDescriptor_ = -1;
}

void ChildProcess::terminate() {
    if (!running_.load()) {
        return;
    }
    if (job_ != nullptr) {
        TerminateJobObject(job_, 1);
    } else if (processHandle_ != nullptr) {
        TerminateProcess(processHandle_, 1);
    }
}

void ChildProcess::readLoop() {
    HANDLE process = processHandle_;
    HANDLE job = job_;
    HANDLE output = outputHandle_;
    HANDLE errorPipe = errorHandle_;
    std::thread errorReader([this, errorPipe] {
        char chunk[16 * 1024];
        while (true) {
            DWORD count = 0;
            if (!ReadFile(errorPipe, chunk, sizeof chunk, &count, nullptr) || count == 0) {
                return;
            }
            if (onErrorOutput) {
                onErrorOutput(std::string_view(chunk, count));
            }
        }
    });
    // Once the program is gone, whatever it left behind is ended after a moment, so pipes a
    // leftover helper holds open can't keep the readers waiting forever.
    std::atomic<bool> readersDone{false};
    std::thread watcher([&readersDone, process, job] {
        WaitForSingleObject(process, INFINITE);
        for (int attempt = 0; attempt < 40 && !readersDone.load(); ++attempt) {
            Sleep(25);
        }
        if (!readersDone.load() && job != nullptr) {
            TerminateJobObject(job, 0);
        }
    });

    std::vector<char> chunk(64 * 1024);
    while (true) {
        DWORD count = 0;
        if (!ReadFile(output, chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr) || count == 0) {
            break;
        }
        if (onOutput) {
            onOutput(std::string_view(chunk.data(), count));
        }
    }
    errorReader.join();
    readersDone.store(true);
    // The watcher returns once the program has ended (ending it if the pipes closed first).
    if (WaitForSingleObject(process, 3000) == WAIT_TIMEOUT && job != nullptr) {
        TerminateJobObject(job, 1);
    }
    watcher.join();

    DWORD exitCode = 0;
    GetExitCodeProcess(process, &exitCode);
    closeHandle(output);
    closeHandle(errorPipe);
    outputHandle_ = nullptr;
    errorHandle_ = nullptr;
    outputDescriptor_ = -1;
    errorDescriptor_ = -1;
    closeInput();
    running_.store(false);
    if (onExit) {
        onExit(static_cast<int>(exitCode));
    }
}

}  // namespace gw

#endif  // _WIN32
