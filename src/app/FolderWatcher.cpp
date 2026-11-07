#include "FolderWatcher.hpp"

#include "Loop.hpp"
#include "Paths.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#ifdef _WIN32
#include "WindowsText.hpp"
#include <windows.h>
#else
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace gw::app {

namespace {

/// Folders whose changes nobody needs to hear about one by one (they change by the thousand).
bool isNoisyFolder(const std::string& name) {
    static const std::set<std::string> noisy = {".git", "node_modules", ".cache", "__pycache__", ".venv", "target", ".gradle"};
    return noisy.count(name) > 0;
}

}  // namespace

FolderWatcher::FolderWatcher(std::string folder, std::function<void(std::vector<std::string>)> onChange)
    : folder_(std::move(folder)), onChange_(std::move(onChange)) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(widenText(folder_).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    handle_ = handle;
#else
    descriptor_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (descriptor_ < 0 || pipe(wakeDescriptor_) != 0) {
        return;
    }
#endif
    thread_ = std::thread([this] { run(); });
}

FolderWatcher::~FolderWatcher() {
    alive_.reset();
    stopping_.store(true);
#ifdef _WIN32
    if (handle_ != nullptr) {
        CancelIoEx(static_cast<HANDLE>(handle_), nullptr);
    }
#else
    if (wakeDescriptor_[1] >= 0) {
        char byte = 1;
        (void)write(wakeDescriptor_[1], &byte, 1);
    }
#endif
    if (thread_.joinable()) {
        thread_.join();
    }
#ifdef _WIN32
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
#else
    if (descriptor_ >= 0) {
        close(descriptor_);
    }
    for (int end : wakeDescriptor_) {
        if (end >= 0) {
            close(end);
        }
    }
#endif
}

void FolderWatcher::report(std::vector<std::string> paths) {
    if (paths.empty()) {
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    runOnMain([this, alive, paths = std::move(paths)] {
        if (!alive.expired() && onChange_) {
            onChange_(paths);
        }
    });
}

#ifdef _WIN32

void FolderWatcher::run() {
    HANDLE folder = static_cast<HANDLE>(handle_);
    std::vector<DWORD> buffer(64 * 1024 / sizeof(DWORD));
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::set<std::string> pending;
    auto lastEvent = std::chrono::steady_clock::now();
    bool reading = false;
    std::string base = folder_;
    while (!stopping_.load()) {
        if (!reading) {
            ResetEvent(overlapped.hEvent);
            DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                           FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;
            if (!ReadDirectoryChangesW(folder, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)), TRUE, filter,
                                       nullptr, &overlapped, nullptr)) {
                break;
            }
            reading = true;
        }
        // Changes come in bursts (a save is several events, a build hundreds): wait for a pause.
        DWORD wait = pending.empty() ? 500 : 60;
        DWORD result = WaitForSingleObject(overlapped.hEvent, wait);
        if (result == WAIT_OBJECT_0) {
            reading = false;
            DWORD bytes = 0;
            if (!GetOverlappedResult(folder, &overlapped, &bytes, FALSE)) {
                if (stopping_.load()) {
                    break;
                }
                continue;
            }
            if (bytes == 0) {
                pending.insert(base);  // too much changed to list: everything may have
            }
            auto* entry = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
            while (bytes > 0) {
                std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
                std::string relative = narrowText(name);
                std::string first = relative.substr(0, relative.find('\\'));
                if (!isNoisyFolder(first) || relative.find("\\") == std::string::npos) {
                    pending.insert(base + "\\" + relative);
                }
                if (entry->NextEntryOffset == 0) {
                    break;
                }
                entry = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<char*>(entry) + entry->NextEntryOffset);
            }
            lastEvent = std::chrono::steady_clock::now();
            continue;
        }
        if (!pending.empty() && std::chrono::steady_clock::now() - lastEvent > std::chrono::milliseconds(80)) {
            report(std::vector<std::string>(pending.begin(), pending.end()));
            pending.clear();
        }
    }
    if (reading) {
        CancelIoEx(folder, &overlapped);
        DWORD bytes = 0;
        GetOverlappedResult(folder, &overlapped, &bytes, TRUE);
    }
    CloseHandle(overlapped.hEvent);
}

#else

void FolderWatcher::run() {
    std::map<int, std::string> folders;
    constexpr uint32_t kEvents = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB;
    auto watch = [&](const std::string& folder) {
        int id = inotify_add_watch(descriptor_, folder.c_str(), kEvents);
        if (id >= 0) {
            folders[id] = folder;
        }
    };
    // inotify watches one folder at a time: add every folder below (up to a limit).
    watch(folder_);
    std::error_code error;
    size_t count = 0;
    for (auto entry = std::filesystem::recursive_directory_iterator(folder_, std::filesystem::directory_options::skip_permission_denied, error);
         entry != std::filesystem::recursive_directory_iterator() && count < 8000; entry.increment(error)) {
        if (error) {
            break;
        }
        if (entry->is_directory(error)) {
            if (isNoisyFolder(entry->path().filename().string())) {
                entry.disable_recursion_pending();
                continue;
            }
            watch(entry->path().string());
            ++count;
        }
    }
    std::set<std::string> pending;
    auto lastEvent = std::chrono::steady_clock::now();
    std::vector<char> buffer(64 * 1024);
    while (!stopping_.load()) {
        pollfd watched[2] = {{descriptor_, POLLIN, 0}, {wakeDescriptor_[0], POLLIN, 0}};
        int ready = poll(watched, 2, pending.empty() ? 500 : 60);
        if (watched[1].revents & POLLIN) {
            break;
        }
        if (ready > 0 && (watched[0].revents & POLLIN)) {
            ssize_t length = read(descriptor_, buffer.data(), buffer.size());
            for (ssize_t offset = 0; offset < length;) {
                auto* event = reinterpret_cast<inotify_event*>(buffer.data() + offset);
                auto folder = folders.find(event->wd);
                if (folder != folders.end()) {
                    std::string path = folder->second + (event->len > 0 ? "/" + std::string(event->name) : std::string());
                    pending.insert(path);
                    if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR) && event->len > 0 && !isNoisyFolder(event->name)) {
                        watch(path);
                    }
                }
                if (event->mask & IN_IGNORED) {
                    folders.erase(event->wd);
                }
                offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
            }
            lastEvent = std::chrono::steady_clock::now();
            continue;
        }
        if (!pending.empty() && std::chrono::steady_clock::now() - lastEvent > std::chrono::milliseconds(80)) {
            report(std::vector<std::string>(pending.begin(), pending.end()));
            pending.clear();
        }
    }
}

#endif

}  // namespace gw::app
