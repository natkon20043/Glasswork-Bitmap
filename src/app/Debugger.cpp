#include "Debugger.hpp"

#include "JsonText.hpp"
#include "Loop.hpp"

#include <algorithm>
#include <regex>

namespace gw::app {

namespace {

int64_t integerOr(const dap::Json& object, const char* key, int64_t fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) {
        return fallback;
    }
    return found->get<int64_t>();
}

bool booleanOr(const dap::Json& object, const char* key, bool fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    auto found = object.find(key);
    return found != object.end() && found->is_boolean() ? found->get<bool>() : fallback;
}

/// The function named in GDB's "info line" answer ("... starts at address 0x1149 <main+8> and ..."):
/// "main". Empty when it names none.
std::string functionInLineInfo(const std::string& text) {
    static const std::regex pattern(R"(<([^+>]+)(\+\d+)?>)");
    std::smatch match;
    return std::regex_search(text, match, pattern) ? match[1].str() : std::string();
}

/// A file name for GDB's explicit locations (-source), quoted so spaces stay part of it.
std::string gdbQuoted(const std::string& path) {
    std::string quoted = "\"";
    for (char character : path) {
        if (character == '"' || character == '\\') {
            quoted += '\\';
        }
        quoted += character;
    }
    return quoted + "\"";
}

}  // namespace

Debugger::Debugger() = default;

Debugger::~Debugger() {
    alive_.reset();
    if (connection_) {
        connection_->stop();
    }
}

void Debugger::setState(DebugState state) {
    state_ = state;
    if (onChange) {
        onChange();
    }
}

bool Debugger::supports(const char* capability) const {
    return booleanOr(capabilities_, capability, false);
}

void Debugger::startProgram(const std::string& program, const std::vector<std::string>& arguments, const std::string& directory,
                            const std::map<std::string, Breakpoints>& breakpoints) {
    if (state_ != DebugState::Idle) {
        return;
    }
    std::vector<std::string> command = dap::defaultAdapterCommand();
    if (command.empty()) {
        if (onError) {
#ifdef _WIN32
            onError("No debugger was found. Install GDB (MSYS2: pacman -S mingw-w64-ucrt-x86_64-gdb) or LLVM's lldb-dap.");
#else
            onError("No debugger was found. Install GDB 14 or newer, or LLVM's lldb-dap.");
#endif
        }
        return;
    }
    bool gdb = dap::adapterIsGdb(command);
    dap::Json args = dap::Json::array();
    for (const std::string& argument : arguments) {
        args.push_back(argument);
    }
    dap::Json launch = {{"program", program}, {"args", args}, {"cwd", directory}};
    if (!gdb) {
        launch["stopOnEntry"] = false;
    }
    startSession(command, gdb ? "gdb" : "lldb-dap", launch, directory, breakpoints);
}

void Debugger::startPython(const std::string& script, const std::vector<std::string>& arguments, const std::string& directory,
                           const std::string& interpreter, const std::string& adapterPython,
                           const std::map<std::string, Breakpoints>& breakpoints) {
    if (state_ != DebugState::Idle) {
        return;
    }
    dap::Json args = dap::Json::array();
    for (const std::string& argument : arguments) {
        args.push_back(argument);
    }
    // The program's output comes back as output events (the debugger's console), and only the
    // user's own code is stepped through, not the standard library's.
    dap::Json launch = {{"program", script},       {"args", args},          {"cwd", directory},
                        {"console", "internalConsole"}, {"justMyCode", true}, {"stopOnEntry", false},
                        {"python", dap::Json::array({interpreter})}};
    startSession({adapterPython, "-m", "debugpy.adapter"}, "debugpy", launch, directory, breakpoints);
}

void Debugger::startSession(const std::vector<std::string>& command, const std::string& adapterName, const dap::Json& launch,
                            const std::string& directory, const std::map<std::string, Breakpoints>& breakpoints) {
    breakpoints_ = breakpoints;
    statuses_.clear();
    breakpointIds_.clear();
    movedTo_.clear();
    runTarget_.reset();
    capabilities_ = dap::Json::object();
    warnedUnsupported_ = false;
    frames_.clear();
    selected_ = 0;
    threadId_ = 0;
    endedReported_ = false;
    exitCode_ = 0;
    uint64_t generation = ++generation_;
    gdb_ = adapterName == "gdb";
    adapterName_ = adapterName;
    setState(DebugState::Starting);

    connection_ = std::make_unique<dap::Connection>();
    std::weak_ptr<bool> alive = alive_;
    connection_->onEvent = [this, alive, generation](const std::string& event, const dap::Json& body) {
        auto shared = std::make_shared<dap::Json>(body);
        std::string name = event;
        runOnMain([this, alive, generation, shared, name] {
            if (!alive.expired() && generation_ == generation) {
                handleEvent(name, *shared);
            }
        });
    };
    connection_->onExit = [this, alive, generation](int) {
        runOnMain([this, alive, generation] {
            if (!alive.expired() && generation_ == generation) {
                endSession(-1, true);
            }
        });
    };
    std::string error;
    if (!connection_->start(command, directory, &error)) {
        connection_.reset();
        setState(DebugState::Idle);
        if (onError) {
            onError("Couldn't start the debugger: " + error);
        }
        return;
    }
    dap::Json initialize = {
        {"adapterID", adapterName_}, {"clientID", "glasswork-bitmap"}, {"clientName", "Glasswork Bitmap"},
        {"linesStartAt1", true}, {"columnsStartAt1", true}, {"pathFormat", "path"},
        {"supportsRunInTerminalRequest", false},
    };
    connection_->request("initialize", initialize,
                         [this, alive, generation, launch](bool success, const dap::Json& body, const std::string& message) {
        std::string text = message;
        auto capabilities = std::make_shared<dap::Json>(body);
        runOnMain([=, this] {
            if (alive.expired() || generation_ != generation) {
                return;
            }
            if (!success) {
                fail("The debugger didn't start: " + text);
                return;
            }
            if (capabilities->is_object()) {
                capabilities_ = *capabilities;
            }
            sendLaunch(launch);
        });
    });
}

void Debugger::sendLaunch(const dap::Json& launch) {
    if (!connection_) {
        return;
    }
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    // The answer to launch only comes after configurationDone, so it is just watched for errors.
    connection_->request("launch", launch, [this, alive, generation](bool success, const dap::Json&, const std::string& message) {
        if (success) {
            return;
        }
        std::string text = message;
        runOnMain([this, alive, generation, text] {
            if (!alive.expired() && generation_ == generation) {
                fail("The program couldn't be started: " + text);
            }
        });
    });
}

void Debugger::fail(const std::string& message) {
    if (onError) {
        onError(message);
    }
    endSession(-1, false);
}

void Debugger::endSession(long code, bool report) {
    ++generation_;
    if (connection_) {
        connection_->stop();
        connection_.reset();
    }
    frames_.clear();
    selected_ = 0;
    statuses_.clear();
    breakpointIds_.clear();
    movedTo_.clear();
    runTarget_.reset();
    setState(DebugState::Idle);
    if (onBreakpointStatus) {
        onBreakpointStatus();
    }
    if (report && !endedReported_ && onEnded) {
        endedReported_ = true;
        onEnded(code);
    }
}

void Debugger::stop() {
    if (state_ != DebugState::Idle) {
        endSession(-1, true);
    }
}

void Debugger::handleEvent(const std::string& event, const dap::Json& body) {
    if (event == "initialized") {
        sendAllBreakpointsThenConfigurationDone();
    } else if (event == "stopped") {
        threadId_ = integerOr(body, "threadId", threadId_);
        if (runTarget_) {
            // Run to Cursor is over, however the program came to stop: its breakpoint goes.
            std::string path = runTarget_->first;
            runTarget_.reset();
            sendBreakpoints(path);
        }
        loadStackAfterStop();
    } else if (event == "continued") {
        frames_.clear();
        setState(DebugState::Running);
    } else if (event == "breakpoint") {
        // The debugger placed (or moved, or dropped) a breakpoint later: GDB places those in a program
        // only once the program is loaded.
        if (!body.contains("breakpoint") || !body["breakpoint"].is_object() || !body["breakpoint"].contains("id")) {
            return;
        }
        const dap::Json& breakpoint = body["breakpoint"];
        auto found = breakpointIds_.find(integerOr(breakpoint, "id", -1));
        if (found == breakpointIds_.end()) {
            return;  // one of the debugger's own (a temporary breakpoint), or from an older request
        }
        auto [path, requested] = found->second;
        if (stringOr(body, "reason") == "removed") {
            statuses_[path].erase(requested);
            breakpointIds_.erase(found);
        } else {
            updateStatus(breakpoint, path, requested);
            reportMoves(path);
        }
        if (onBreakpointStatus) {
            onBreakpointStatus();
        }
    } else if (event == "output") {
        std::string text = stringOr(body, "output");
        std::string category = stringOr(body, "category", "console");
        if (category == "telemetry") {
            return;
        }
        // The debugger's own messages (a logpoint's among them) are whole lines, even when they
        // come without their line end; the program's output is passed on exactly as it was written.
        if (category == "console" && !text.empty() && text.back() != '\n') {
            text += '\n';
        }
        if (onOutput && !text.empty()) {
            onOutput(text, category);
        }
    } else if (event == "exited") {
        exitCode_ = static_cast<long>(integerOr(body, "exitCode", 0));
        if (onOutput) {
            onOutput("The program exited with code " + std::to_string(exitCode_) + ".\n", "console");
        }
    } else if (event == "terminated") {
        endSession(exitCode_, true);
    }
}

void Debugger::sendAllBreakpointsThenConfigurationDone() {
    if (!connection_) {
        return;
    }
    for (const auto& [path, breakpoints] : breakpoints_) {
        sendBreakpoints(path);
    }
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    connection_->request("configurationDone", dap::Json::object(), [this, alive, generation](bool, const dap::Json&, const std::string&) {
        runOnMain([this, alive, generation] {
            if (!alive.expired() && generation_ == generation && state_ == DebugState::Starting) {
                setState(DebugState::Running);
            }
        });
    });
}

void Debugger::sendBreakpoints(const std::string& path) {
    if (!connection_) {
        return;
    }
    // Every enabled breakpoint of the file, in line order; `requested` remembers which line each
    // was, since the answers come back in the same order.
    dap::Json list = dap::Json::array();
    std::vector<size_t> requested;
    std::set<size_t> mine;
    bool unsupported = false;
    auto found = breakpoints_.find(path);
    if (found != breakpoints_.end()) {
        for (const auto& [line, settings] : found->second) {
            if (!settings.enabled) {
                continue;
            }
            dap::Json item = {{"line", line + 1}};
            auto add = [&](const char* capability, const char* key, const std::string& value) {
                if (value.empty()) {
                    return;
                }
                if (supports(capability)) {
                    item[key] = value;
                } else {
                    unsupported = true;
                }
            };
            add("supportsConditionalBreakpoints", "condition", settings.condition);
            add("supportsHitConditionalBreakpoints", "hitCondition", settings.hitCondition);
            add("supportsLogPoints", "logMessage", settings.logMessage);
            list.push_back(item);
            requested.push_back(line);
            mine.insert(line);
        }
    }
    // Run to Cursor stops at its line once, unless a breakpoint of the user's is there already.
    if (runTarget_ && runTarget_->first == path && mine.count(runTarget_->second) == 0) {
        list.push_back({{"line", runTarget_->second + 1}});
        requested.push_back(runTarget_->second);
    }
    if (unsupported && !warnedUnsupported_ && onOutput) {
        warnedUnsupported_ = true;
        onOutput(adapterName_ + " can't use some of these breakpoints' conditions, hit counts or log messages: those stop every time.\n",
                 "console");
    }
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    connection_->request("setBreakpoints", {{"source", {{"path", path}}}, {"breakpoints", list}},
                         [this, alive, generation, path, requested, mine](bool success, const dap::Json& body, const std::string&) {
        auto shared = std::make_shared<dap::Json>(body);
        runOnMain([this, alive, generation, path, requested, mine, success, shared] {
            if (alive.expired() || generation_ != generation) {
                return;
            }
            // This answer replaces everything said about the file before.
            statuses_.erase(path);
            for (auto entry = breakpointIds_.begin(); entry != breakpointIds_.end();) {
                entry = entry->second.first == path ? breakpointIds_.erase(entry) : std::next(entry);
            }
            if (success && shared->contains("breakpoints") && (*shared)["breakpoints"].is_array()) {
                const dap::Json& results = (*shared)["breakpoints"];
                for (size_t index = 0; index < requested.size() && index < results.size(); ++index) {
                    if (mine.count(requested[index]) > 0) {
                        updateStatus(results[index], path, requested[index]);
                    }
                }
            }
            reportMoves(path);
            if (onBreakpointStatus) {
                onBreakpointStatus();
            }
        });
    });
}

void Debugger::updateStatus(const dap::Json& breakpoint, const std::string& path, size_t requested) {
    BreakpointStatus status;
    status.verified = booleanOr(breakpoint, "verified", false);
    int64_t line = integerOr(breakpoint, "line", 0);
    status.line = line > 0 ? static_cast<long>(line - 1) : -1;
    status.message = stringOr(breakpoint, "message");
    statuses_[path][requested] = status;
    if (breakpoint.is_object() && breakpoint.contains("id") && breakpoint["id"].is_number_integer()) {
        breakpointIds_[breakpoint["id"].get<int64_t>()] = {path, requested};
    }
}

void Debugger::reportMoves(const std::string& path) {
    if (!onBreakpointMoved) {
        return;
    }
    auto statuses = statuses_.find(path);
    auto mine = breakpoints_.find(path);
    if (statuses == statuses_.end() || mine == breakpoints_.end()) {
        return;
    }
    // Each breakpoint is moved once at most, so a debugger that keeps changing its mind can't
    // send one back and forth.
    std::vector<std::pair<size_t, size_t>> moves;
    for (const auto& [requested, status] : statuses->second) {
        bool elsewhere = status.verified && status.line >= 0 && static_cast<size_t>(status.line) != requested;
        if (elsewhere && mine->second.count(requested) > 0 && movedTo_.count({path, requested}) == 0) {
            moves.emplace_back(requested, static_cast<size_t>(status.line));
        }
    }
    for (const auto& [from, to] : moves) {
        movedTo_.insert({path, to});
        onBreakpointMoved(path, from, to);
    }
}

void Debugger::setBreakpoints(const std::string& path, const Breakpoints& breakpoints) {
    if (breakpoints.empty()) {
        breakpoints_.erase(path);
    } else {
        breakpoints_[path] = breakpoints;
    }
    if (connection_ && state_ != DebugState::Idle) {
        sendBreakpoints(path);
    }
}

std::optional<BreakpointStatus> Debugger::breakpointStatus(const std::string& path, size_t line) const {
    auto file = statuses_.find(path);
    if (file == statuses_.end()) {
        return std::nullopt;
    }
    auto found = file->second.find(line);
    if (found == file->second.end()) {
        return std::nullopt;
    }
    return found->second;
}

void Debugger::loadStackAfterStop() {
    if (!connection_) {
        return;
    }
    if (threadId_ != 0) {
        loadStack(threadId_);
        return;
    }
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    connection_->request("threads", dap::Json::object(), [this, alive, generation](bool success, const dap::Json& body, const std::string&) {
        int64_t first = 1;
        if (success && body.contains("threads") && body["threads"].is_array() && !body["threads"].empty()) {
            first = integerOr(body["threads"][0], "id", 1);
        }
        runOnMain([this, alive, generation, first] {
            if (!alive.expired() && generation_ == generation) {
                loadStack(first);
            }
        });
    });
}

void Debugger::loadStack(int64_t thread) {
    if (!connection_) {
        return;
    }
    threadId_ = thread;
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    connection_->request("stackTrace", {{"threadId", thread}, {"levels", 60}},
                         [this, alive, generation](bool success, const dap::Json& body, const std::string&) {
        auto shared = std::make_shared<dap::Json>(body);
        runOnMain([this, alive, generation, success, shared] {
            if (alive.expired() || generation_ != generation) {
                return;
            }
            frames_.clear();
            if (success && shared->contains("stackFrames") && (*shared)["stackFrames"].is_array()) {
                for (const dap::Json& item : (*shared)["stackFrames"]) {
                    DebugFrame frame;
                    frame.id = integerOr(item, "id", 0);
                    frame.name = stringOr(item, "name");
                    frame.line = static_cast<long>(integerOr(item, "line", 0));
                    if (item.contains("source") && item["source"].is_object()) {
                        frame.path = stringOr(item["source"], "path");
                    }
                    frames_.push_back(frame);
                }
            }
            // Stop on the first frame with source code to show.
            selected_ = 0;
            for (size_t index = 0; index < frames_.size(); ++index) {
                if (!frames_[index].path.empty()) {
                    selected_ = index;
                    break;
                }
            }
            setState(DebugState::Stopped);
            if (!frames_.empty() && onLocation) {
                onLocation(frames_[selected_]);
            }
        });
    });
}

void Debugger::selectFrame(size_t index) {
    if (index >= frames_.size()) {
        return;
    }
    selected_ = index;
    if (onChange) {
        onChange();
    }
    if (onLocation) {
        onLocation(frames_[index]);
    }
}

void Debugger::step(const char* command) {
    if (!connection_ || state_ != DebugState::Stopped) {
        return;
    }
    frames_.clear();
    setState(DebugState::Running);
    connection_->request(command, {{"threadId", threadId_}}, [](bool, const dap::Json&, const std::string&) {});
}

void Debugger::resume() { step("continue"); }
void Debugger::stepOver() { step("next"); }
void Debugger::stepInto() { step("stepIn"); }
void Debugger::stepOut() { step("stepOut"); }

void Debugger::pause() {
    if (!connection_ || state_ != DebugState::Running) {
        return;
    }
    connection_->request("pause", {{"threadId", threadId_ == 0 ? 1 : threadId_}}, [](bool, const dap::Json&, const std::string&) {});
}

void Debugger::runToLine(const std::string& path, size_t line) {
    if (!connection_ || state_ != DebugState::Stopped) {
        return;
    }
    // A breakpoint that lasts until the program next stops (handleEvent takes it away), then on.
    runTarget_ = std::make_pair(path, line);
    sendBreakpoints(path);
    resume();
}

bool Debugger::canJump() const {
    return connection_ != nullptr && (gdb_ || supports("supportsGotoTargetsRequest"));
}

void Debugger::jumpToLine(const std::string& path, size_t line) {
    if (!connection_ || state_ != DebugState::Stopped || frames_.empty()) {
        return;
    }
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    std::string lineText = std::to_string(line + 1);
    auto refuse = [this, alive, generation, lineText](const std::string& why) {
        runOnMain([this, alive, generation, lineText, why] {
            if (!alive.expired() && generation_ == generation && onError) {
                onError("Can't jump to line " + lineText + ": " + why);
            }
        });
    };
    if (supports("supportsGotoTargetsRequest")) {
        // The adapter's own way: where on that line it can go, then go there.
        int64_t thread = threadId_;
        connection_->request("gotoTargets", {{"source", {{"path", path}}}, {"line", line + 1}},
                             [this, alive, generation, thread, refuse](bool success, const dap::Json& body, const std::string& message) {
            if (!success || !body.contains("targets") || !body["targets"].is_array() || body["targets"].empty()) {
                refuse(message.empty() ? std::string("there is no code there to go to.") : message);
                return;
            }
            int64_t target = integerOr(body["targets"][0], "id", 0);
            runOnMain([this, alive, generation, thread, target, refuse] {
                if (alive.expired() || generation_ != generation || !connection_) {
                    return;
                }
                connection_->request("goto", {{"threadId", thread}, {"targetId", target}},
                                     [refuse](bool success, const dap::Json&, const std::string& message) {
                    if (!success) {
                        refuse(message.empty() ? std::string("the debugger refused.") : message);
                    }
                });
            });
        });
        return;
    }
    if (!gdb_) {
        refuse("this debugger can't move to another line.");
        return;
    }
    // GDB: its jump command, only within the function that is running (jumping into another one
    // would run its code with this one's stack). A temporary breakpoint there stops it at once.
    int64_t frame = frames_.front().id;
    std::string location = "-source " + gdbQuoted(path) + " -line " + lineText;
    auto ask = [this, frame](const std::string& command, dap::Connection::ResponseHandler handler) {
        connection_->request("evaluate", {{"expression", command}, {"context", "repl"}, {"frameId", frame}}, std::move(handler));
    };
    ask("info line *$pc", [this, alive, generation, location, ask, refuse](bool success, const dap::Json& body, const std::string&) {
        std::string here = success ? functionInLineInfo(stringOr(body, "result")) : std::string();
        runOnMain([this, alive, generation, location, ask, refuse, here] {
            if (alive.expired() || generation_ != generation || !connection_) {
                return;
            }
            ask("info line " + location, [this, alive, generation, location, ask, refuse, here](bool success, const dap::Json& body,
                                                                                                const std::string& message) {
                std::string there = success ? functionInLineInfo(stringOr(body, "result")) : std::string();
                if (!success || there.empty()) {
                    refuse(message.empty() ? std::string("GDB found no code on that line.") : message);
                    return;
                }
                if (here.empty() || there != here) {
                    refuse("that line is in " + there + ", and only lines of the function that is running (" +
                           (here.empty() ? std::string("unknown") : here) + ") can be jumped to.");
                    return;
                }
                runOnMain([this, alive, generation, location, ask, refuse] {
                    if (alive.expired() || generation_ != generation || !connection_) {
                        return;
                    }
                    ask("tbreak " + location, [](bool, const dap::Json&, const std::string&) {});
                    ask("jump " + location, [refuse](bool success, const dap::Json&, const std::string& message) {
                        if (!success) {
                            refuse(message.empty() ? std::string("GDB refused.") : message);
                        }
                    });
                });
            });
        });
    });
}

std::shared_ptr<DebugVariable> Debugger::variableFrom(const dap::Json& item) {
    auto variable = std::make_shared<DebugVariable>();
    variable->name = stringOr(item, "name");
    variable->value = stringOr(item, "value");
    variable->type = stringOr(item, "type");
    variable->reference = integerOr(item, "variablesReference", 0);
    return variable;
}

void Debugger::fetchScopes(std::function<void(std::vector<std::shared_ptr<DebugVariable>>)> done) {
    if (!connection_ || state_ != DebugState::Stopped || frames_.empty()) {
        done({});
        return;
    }
    int64_t frameId = frames_[selected_].id;
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    connection_->request("scopes", {{"frameId", frameId}}, [this, alive, generation, done](bool success, const dap::Json& body, const std::string&) {
        auto shared = std::make_shared<dap::Json>(body);
        runOnMain([this, alive, generation, done, success, shared] {
            if (alive.expired() || generation_ != generation) {
                return;
            }
            std::vector<std::shared_ptr<DebugVariable>> scopes;
            if (success && shared->contains("scopes") && (*shared)["scopes"].is_array()) {
                for (const dap::Json& item : (*shared)["scopes"]) {
                    auto scope = variableFrom(item);
                    scope->value.clear();
                    scopes.push_back(scope);
                }
            }
            done(scopes);
        });
    });
}

void Debugger::fetchChildren(std::shared_ptr<DebugVariable> variable, std::function<void()> done) {
    if (!connection_ || variable->reference == 0) {
        variable->fetched = true;
        done();
        return;
    }
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    connection_->request("variables", {{"variablesReference", variable->reference}},
                         [this, alive, generation, variable, done](bool success, const dap::Json& body, const std::string&) {
        auto shared = std::make_shared<dap::Json>(body);
        runOnMain([this, alive, generation, variable, done, success, shared] {
            if (alive.expired() || generation_ != generation) {
                return;
            }
            variable->children.clear();
            if (success && shared->contains("variables") && (*shared)["variables"].is_array()) {
                for (const dap::Json& item : (*shared)["variables"]) {
                    variable->children.push_back(variableFrom(item));
                }
            }
            variable->fetched = true;
            done();
        });
    });
}

void Debugger::evaluate(const std::string& expression, std::function<void(std::string, std::string)> done) {
    if (!connection_ || state_ == DebugState::Idle) {
        done("", "Start debugging first.");
        return;
    }
    dap::Json arguments = {{"expression", expression}, {"context", "repl"}};
    if (state_ == DebugState::Stopped && !frames_.empty()) {
        arguments["frameId"] = frames_[selected_].id;
    }
    uint64_t generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    connection_->request("evaluate", arguments, [this, alive, generation, done](bool success, const dap::Json& body, const std::string& message) {
        std::string result = stringOr(body, "result");
        std::string error = message;
        runOnMain([this, alive, generation, done, success, result, error] {
            if (alive.expired() || generation_ != generation) {
                return;
            }
            if (success) {
                done(result, "");
            } else {
                done("", error.empty() ? std::string("No result.") : error);
            }
        });
    });
}

}  // namespace gw::app
