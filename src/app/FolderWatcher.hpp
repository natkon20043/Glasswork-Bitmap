#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gw::app {

/// Watches a folder and everything below it, and reports the paths that changed, a little after
/// they did, on the main thread. ReadDirectoryChangesW on Windows, inotify on Linux.
class FolderWatcher {
public:
    FolderWatcher(std::string folder, std::function<void(std::vector<std::string> paths)> onChange);
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    const std::string& folder() const { return folder_; }

private:
    void run();
    void report(std::vector<std::string> paths);

    std::string folder_;
    std::function<void(std::vector<std::string>)> onChange_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    void* handle_ = nullptr;  // Windows: the folder; Linux: unused
    int descriptor_ = -1;     // Linux: inotify
    int wakeDescriptor_[2] = {-1, -1};
};

}  // namespace gw::app
