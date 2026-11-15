#pragma once

#include "Dap.hpp"
#include "Document.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
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

/// What the debugger made of a breakpoint.
struct BreakpointStatus {
    /// It could be placed (the program has code there, the file is part of it).
    bool verified = false;
    /// The line (zero-based) it really stops at, which can be below the line it was put on; -1 if
    /// the debugger didn't say.
    long line = -1;
    /// Why it couldn't be placed, when the debugger says.
    std::string message;
};

/// A debugging session through a debug adapter: lldb-dap when it is installed, else GDB's own
/// (gdb --interpreter=dap), for programs; debugpy for Python. Main thread only; callbacks arrive
/// there too.
class Debugger {
public:
    Debugger();
    ~Debugger();

    // ---- Session
    /// A compiled program (C, C++, Rust...). `breakpoints` maps files to their breakpoints.
    void startProgram(const std::string& program, const std::vector<std::string>& arguments, const std::string& directory,
                      const std::map<std::string, Breakpoints>& breakpoints);
    /// A Python script run by `interpreter`, through debugpy's adapter run by `adapterPython` (a
    /// Python that has debugpy; it can be the same one).
    void startPython(const std::string& script, const std::vector<std::string>& arguments, const std::string& directory,
                     const std::string& interpreter, const std::string& adapterPython,
                     const std::map<std::string, Breakpoints>& breakpoints);
    void stop();
    void resume();
    void pause();
    void stepOver();
    void stepInto();
    void stepOut();
    /// Carries on until `line` (zero-based) of `path` is reached, or something else stops it first.
    /// Only while stopped.
    void runToLine(const std::string& path, size_t line);
    /// Moves the next line to run to `line` (zero-based) of `path`, in the function that is running,
    /// without running what is in between. Only while stopped.
    void jumpToLine(const std::string& path, size_t line);
    /// Whether this debugger can jump (debugpy and lldb-dap ask the adapter; GDB through its own
    /// jump command).
    bool canJump() const;

    // ---- Breakpoints
    void setBreakpoints(const std::string& path, const Breakpoints& breakpoints);
    /// What the debugger said about the breakpoint on `line` (zero-based) of `path`; empty when it
    /// hasn't (or nothing is being debugged).
    std::optional<BreakpointStatus> breakpointStatus(const std::string& path, size_t line) const;

    // ---- Inspecting
    void selectFrame(size_t index);
    void fetchScopes(std::function<void(std::vector<std::shared_ptr<DebugVariable>>)> done);
    void fetchChildren(std::shared_ptr<DebugVariable> variable, std::function<void()> done);
    void evaluate(const std::string& expression, std::function<void(std::string result, std::string error)> done);

    // ---- Getters
    DebugState state() const { return state_; }
    const std::vector<DebugFrame>& frames() const { return frames_; }
    size_t selectedFrame() const { return selected_; }
    /// "lldb-dap", "gdb" or "debugpy", once a session has started.
    const std::string& adapterName() const { return adapterName_; }

    // ---- Callbacks
    std::function<void()> onChange;
    std::function<void(const DebugFrame& frame)> onLocation;
    /// `category` is "stdout", "stderr" or "console".
    std::function<void(const std::string& text, const std::string& category)> onOutput;
    std::function<void(const std::string& message)> onError;
    std::function<void(long exitCode)> onEnded;
    /// What the debugger says about the breakpoints changed (see breakpointStatus).
    std::function<void()> onBreakpointStatus;
    /// The debugger placed the breakpoint on line `from` (zero-based) of `path` at line `to`, where
    /// the code really is (a blank line, a comment, or a declaration with no code of its own).
    std::function<void(const std::string& path, size_t from, size_t to)> onBreakpointMoved;

private:
    void startSession(const std::vector<std::string>& command, const std::string& adapterName, const dap::Json& launch,
                      const std::string& directory, const std::map<std::string, Breakpoints>& breakpoints);
    void setState(DebugState state);
    void sendLaunch(const dap::Json& launch);
    void fail(const std::string& message);
    void endSession(long code, bool report);
    void handleEvent(const std::string& event, const dap::Json& body);
    void sendAllBreakpointsThenConfigurationDone();
    void sendBreakpoints(const std::string& path);
    void updateStatus(const dap::Json& breakpoint, const std::string& path, size_t requested);
    void reportMoves(const std::string& path);
    void loadStackAfterStop();
    void loadStack(int64_t thread);
    void step(const char* command);
    bool supports(const char* capability) const;
    std::shared_ptr<DebugVariable> variableFrom(const dap::Json& item);

    std::unique_ptr<dap::Connection> connection_;
    DebugState state_ = DebugState::Idle;
    std::vector<DebugFrame> frames_;
    size_t selected_ = 0;
    int64_t threadId_ = 0;
    std::map<std::string, Breakpoints> breakpoints_;
    /// What the adapter said, by file and the line each was asked for.
    std::map<std::string, std::map<size_t, BreakpointStatus>> statuses_;
    /// The adapter's breakpoint ids -> the file and line each was asked for (its later events use them).
    std::map<int64_t, std::pair<std::string, size_t>> breakpointIds_;
    /// Lines (by file) breakpoints were moved to, so none is moved twice.
    std::set<std::pair<std::string, size_t>> movedTo_;
    /// Run to Cursor's temporary breakpoint, until the program next stops.
    std::optional<std::pair<std::string, size_t>> runTarget_;
    dap::Json capabilities_ = dap::Json::object();
    bool warnedUnsupported_ = false;
    uint64_t generation_ = 0;
    bool endedReported_ = false;
    long exitCode_ = 0;
    std::string adapterName_;
    bool gdb_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace gw::app
