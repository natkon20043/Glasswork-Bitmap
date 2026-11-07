#pragma once

#include "Dap.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gw::app {

// Debug adapter events -> what the window shows: nothing debugged, starting, running, or stopped
// (frames and variables can be read).
enum class DebugState { Idle, Starting, Running, Stopped };

struct DebugFrame {
    int64_t id = 0;
    std::string name;
    /// Empty when the frame has no source.
    std::string path;
    /// From 1.
    long line = 0;
};

/// A variable or a part of one; children are fetched when it is opened.
struct DebugVariable {
    std::string name;
    std::string value;
    std::string type;
    /// 0 if it has no parts.
    int64_t reference = 0;
    bool fetched = false;
    bool expanded = false;
    std::vector<std::shared_ptr<DebugVariable>> children;
};

/// A debugging session through a debug adapter: lldb-dap when it is installed, else GDB's own
/// (gdb --interpreter=dap). Main thread only; callbacks arrive there too.
class Debugger {
public:
    Debugger();
    ~Debugger();

    // ---- Session
    /// `breakpoints` maps a file to zero-based lines.
    void startProgram(const std::string& program, const std::vector<std::string>& arguments, const std::string& directory,
                      const std::map<std::string, std::set<size_t>>& breakpoints);
    void stop();
    void resume();
    void pause();
    void stepOver();
    void stepInto();
    void stepOut();

    // ---- Breakpoints
    void setBreakpointLines(const std::set<size_t>& lines, const std::string& path);

    // ---- Inspecting
    void selectFrame(size_t index);
    void fetchScopes(std::function<void(std::vector<std::shared_ptr<DebugVariable>>)> done);
    void fetchChildren(std::shared_ptr<DebugVariable> variable, std::function<void()> done);
    void evaluate(const std::string& expression, std::function<void(std::string result, std::string error)> done);

    // ---- Getters
    DebugState state() const { return state_; }
    const std::vector<DebugFrame>& frames() const { return frames_; }
    size_t selectedFrame() const { return selected_; }
    /// "lldb-dap" or "gdb", once a session has started.
    const std::string& adapterName() const { return adapterName_; }

    // ---- Callbacks
    std::function<void()> onChange;
    std::function<void(const DebugFrame& frame)> onLocation;
    /// `category` is "stdout", "stderr" or "console".
    std::function<void(const std::string& text, const std::string& category)> onOutput;
    std::function<void(const std::string& message)> onError;
    std::function<void(long exitCode)> onEnded;

private:
    void setState(DebugState state);
    void sendLaunch(const std::string& program, const std::vector<std::string>& arguments, const std::string& directory);
    void fail(const std::string& message);
    void endSession(long code, bool report);
    void handleEvent(const std::string& event, const dap::Json& body);
    void sendAllBreakpointsThenConfigurationDone();
    void sendBreakpoints(const std::string& path);
    void loadStackAfterStop();
    void loadStack(int64_t thread);
    void step(const char* command);
    std::shared_ptr<DebugVariable> variableFrom(const dap::Json& item);

    std::unique_ptr<dap::Connection> connection_;
    DebugState state_ = DebugState::Idle;
    std::vector<DebugFrame> frames_;
    size_t selected_ = 0;
    int64_t threadId_ = 0;
    std::map<std::string, std::set<size_t>> breakpoints_;
    uint64_t generation_ = 0;
    bool endedReported_ = false;
    long exitCode_ = 0;
    std::string adapterName_;
    bool gdb_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace gw::app
