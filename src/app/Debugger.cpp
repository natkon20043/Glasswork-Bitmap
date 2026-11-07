#include "Debugger.hpp"

#include "JsonText.hpp"
#include "Loop.hpp"

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

void Debugger::startProgram(const std::string& program, const std::vector<std::string>& arguments, const std::string& directory,
                            const std::map<std::string, std::set<size_t>>& breakpoints) {
    if (state_ != DebugState::Idle) {
        return;
    }
    breakpoints_ = breakpoints;
    frames_.clear();
    selected_ = 0;
    threadId_ = 0;
    endedReported_ = false;
    exitCode_ = 0;
    uint64_t generation = ++generation_;
    setState(DebugState::Starting);

    std::vector<std::string> command = dap::defaultAdapterCommand();
    if (command.empty()) {
        setState(DebugState::Idle);
        if (onError) {
#ifdef _WIN32
            onError("No debugger was found. Install GDB (MSYS2: pacman -S mingw-w64-ucrt-x86_64-gdb) or LLVM's lldb-dap.");
#else
            onError("No debugger was found. Install GDB 14 or newer, or LLVM's lldb-dap.");
#endif
        }
        return;
    }
    gdb_ = dap::adapterIsGdb(command);
    adapterName_ = gdb_ ? "gdb" : "lldb-dap";
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
                         [this, alive, generation, program, arguments, directory](bool success, const dap::Json&, const std::string& message) {
        std::string text = message;
        runOnMain([=, this] {
            if (alive.expired() || generation_ != generation) {
                return;
            }
            if (!success) {
                fail("The debugger didn't start: " + text);
                return;
            }
            sendLaunch(program, arguments, directory);
        });
    });
}

void Debugger::sendLaunch(const std::string& program, const std::vector<std::string>& arguments, const std::string& directory) {
    if (!connection_) {
        return;
    }
    dap::Json args = dap::Json::array();
    for (const std::string& argument : arguments) {
        args.push_back(argument);
    }
    dap::Json launch = {{"program", program}, {"args", args}, {"cwd", directory}};
    if (!gdb_) {
        launch["stopOnEntry"] = false;
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
    setState(DebugState::Idle);
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
        loadStackAfterStop();
    } else if (event == "continued") {
        frames_.clear();
        setState(DebugState::Running);
    } else if (event == "output") {
        std::string text = stringOr(body, "output");
        std::string category = stringOr(body, "category", "console");
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
    for (const auto& [path, lines] : breakpoints_) {
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
    dap::Json list = dap::Json::array();
    for (size_t line : breakpoints_[path]) {
        list.push_back({{"line", line + 1}});
    }
    connection_->request("setBreakpoints", {{"source", {{"path", path}}}, {"breakpoints", list}},
                         [](bool, const dap::Json&, const std::string&) {});
}

void Debugger::setBreakpointLines(const std::set<size_t>& lines, const std::string& path) {
    breakpoints_[path] = lines;
    if (connection_ && state_ != DebugState::Idle) {
        sendBreakpoints(path);
    }
    if (lines.empty()) {
        breakpoints_.erase(path);
    }
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
