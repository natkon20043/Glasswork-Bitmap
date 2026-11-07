#include "ExtensionHostService.hpp"

#include "Fuzzy.hpp"
#include "Jsonc.hpp"
#include "Loop.hpp"
#include "Paths.hpp"
#include "Platform.hpp"
#include "Services.hpp"
#include "SettingsLookup.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace gw::app {

namespace fs = std::filesystem;

namespace {

/// A host that stops this many times in a row is not started again.
constexpr int kMostRestarts = 3;

std::string stringOf(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

/// "$(sync~spin) Building" -> "Building": the icons are VS Code's own set.
std::string withoutIconMarkup(const std::string& text) {
    static const std::regex icons(R"(\$\([^)]*\)\s*)");
    return std::regex_replace(text, icons, "");
}

}  // namespace

ExtensionHostService::ExtensionHostService(ExtensionHostDelegate& delegate) : delegate_(delegate) {}

ExtensionHostService::~ExtensionHostService() {
    alive_.reset();
    if (host_) {
        host_->stop();
    }
}

// ---------------------------------------------------------------------------
// What is installed

void ExtensionHostService::buildIndexIfNeeded() {
    if (indexBuilt_) {
        return;
    }
    indexBuilt_ = true;
    index_.clear();
    contributedCommands_.clear();
    for (const Extension& extension : Services::shared().extensions().extensions()) {
        if (!extension.hasCode || extension.id.rfind("vscode.", 0) == 0) {
            continue;  // declarative only, or one of VS Code's own built-ins
        }
        Json manifest = Json::parse(extension.manifestJson, nullptr, false);
        if (!manifest.is_object()) {
            continue;
        }
        IndexEntry entry;
        entry.identifier = extension.id;
        for (const Json& event : manifest.value("activationEvents", Json::array())) {
            if (event.is_string()) {
                entry.activationEvents.insert(event.get<std::string>());
            }
        }
        const Json contributes = manifest.value("contributes", Json::object());
        for (const Json& language : contributes.value("languages", Json::array())) {
            if (language.is_object() && language.contains("id") && language["id"].is_string()) {
                entry.languages.insert(language["id"].get<std::string>());
            }
        }
        for (const std::string& event : entry.activationEvents) {
            entry.runsAtStartup = entry.runsAtStartup || event == "*" || event == "onStartupFinished" ||
                                  event.rfind("workspaceContains:", 0) == 0;
        }
        std::set<std::string> hidden;
        const Json menus = contributes.value("menus", Json::object());
        for (const Json& item : menus.value("commandPalette", Json::array())) {
            if (item.is_object() && item.value("when", "") == "false") {
                hidden.insert(item.value("command", ""));
            }
        }
        for (const Json& command : contributes.value("commands", Json::array())) {
            if (!command.is_object() || !command.contains("command") || !command["command"].is_string()) {
                continue;
            }
            ContributedCommand contributed;
            contributed.identifier = command["command"].get<std::string>();
            std::string title = command.contains("title") && command["title"].is_string() ? command["title"].get<std::string>() : "";
            std::string category = command.contains("category") && command["category"].is_string() ? command["category"].get<std::string>() : "";
            contributed.title = category.empty() ? title : category + ": " + title;
            contributed.hiddenFromPalette = hidden.count(contributed.identifier) > 0 || title.empty();
            contributedCommands_.push_back(std::move(contributed));
        }
        index_.push_back(std::move(entry));
    }
}

std::string ExtensionHostService::titleForCommand(const std::string& identifier) {
    buildIndexIfNeeded();
    for (const ContributedCommand& command : contributedCommands_) {
        if (command.identifier == identifier) {
            return command.title;
        }
    }
    return std::string();
}

bool ExtensionHostService::hasRunnableExtensions() {
    buildIndexIfNeeded();
    return !index_.empty();
}

void ExtensionHostService::extensionsDidChange() {
    indexBuilt_ = false;
}

// ---------------------------------------------------------------------------
// Starting the host

std::string ExtensionHostService::hostScriptPath() const {
    std::string overridden = environmentValue("GLASSWORK_HOST_DIR");
    if (!overridden.empty()) {
        return utf8FromPath(pathFromUtf8(overridden) / "main.js");
    }
    // Installed: beside the program. A development build: the source folder.
    fs::path bundled = programFolder() / "host" / "main.js";
    std::error_code error;
    if (fs::exists(bundled, error)) {
        return utf8FromPath(bundled);
    }
#ifdef GLASSWORK_SOURCE_DIR
    return utf8FromPath(pathFromUtf8(GLASSWORK_SOURCE_DIR) / "host" / "main.js");
#else
    return utf8FromPath(bundled);
#endif
}

Json ExtensionHostService::extensionDescriptions() {
    Json list = Json::array();
    for (const Extension& extension : Services::shared().extensions().extensions()) {
        if (!extension.hasCode || extension.id.rfind("vscode.", 0) == 0) {
            continue;
        }
        Json manifest = Json::parse(extension.manifestJson, nullptr, false);
        if (!manifest.is_object()) {
            continue;
        }
        list.push_back({{"id", extension.id}, {"path", utf8FromPath(extension.root)}, {"manifest", manifest}});
    }
    return list;
}

Json ExtensionHostService::configuration() {
    Json merged = Json::object();
    for (const fs::path& file : {Services::shared().settingsPath(), Services::shared().supportFolder() / "extension-settings.json"}) {
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            continue;
        }
        std::stringstream text;
        text << input.rdbuf();
        Json parsed = parseJsonc(text.str());
        if (!parsed.is_object()) {
            continue;
        }
        for (const auto& [key, value] : parsed.items()) {
            merged[key] = value;
        }
    }
    return merged;
}

Json ExtensionHostService::allSettings() {
    return configuration();
}

Json ExtensionHostService::languageByExtension() {
    Json map = Json::object();
    for (const Extension& extension : Services::shared().extensions().extensions()) {
        for (const LanguageInfo& language : extension.languages) {
            for (const std::string& suffix : language.extensions) {
                map[suffix] = language.id;
            }
        }
    }
    return map;
}

void ExtensionHostService::ensureHost(std::function<void(bool)> done) {
    if (initialized_ && host_ && host_->isRunning()) {
        done(true);
        return;
    }
    buildIndexIfNeeded();
    if (index_.empty() || restarts_ >= kMostRestarts) {
        done(false);
        return;
    }
    waiting_.push_back(std::move(done));
    if (initializing_) {
        return;
    }
    initializing_ = true;
    stopping_ = false;

    auto host = std::make_unique<ExtensionHost>();
    ExtensionHost* rawHost = host.get();
    std::weak_ptr<bool> alive = alive_;
    host->onNotification = [this, alive, rawHost](const std::string& method, const Json& params) {
        auto sharedMethod = std::make_shared<std::string>(method);
        auto sharedParams = std::make_shared<Json>(params);
        runOnMain([this, alive, rawHost, sharedMethod, sharedParams] {
            if (alive.expired() || host_.get() != rawHost) {
                return;
            }
            try {
                handleNotification(*sharedMethod, *sharedParams);
            } catch (const std::exception& problem) {
                std::fprintf(stderr, "Glasswork Bitmap: dropped %s from the extension host: %s\n", sharedMethod->c_str(), problem.what());
            }
        });
    };
    host->onRequest = [this, alive, rawHost](const std::string& method, const Json& params, ExtensionHost::Reply reply) {
        auto sharedMethod = std::make_shared<std::string>(method);
        auto sharedParams = std::make_shared<Json>(params);
        runOnMain([this, alive, rawHost, sharedMethod, sharedParams, reply] {
            if (alive.expired() || host_.get() != rawHost) {
                reply(Json(), "The extension host was restarted");
                return;
            }
            try {
                handleRequest(*sharedMethod, *sharedParams, reply);
            } catch (const std::exception& problem) {
                reply(Json(), std::string("Glasswork Bitmap couldn't handle this request: ") + problem.what());
            }
        });
    };
    host->onStderr = [this, alive](std::string_view bytes) {
        std::string text(bytes);
        runOnMain([this, alive, text] {
            if (!alive.expired()) {
                appendLog(text, "Extension Host");
            }
        });
    };
    host->onExit = [this, alive, rawHost](int code) {
        runOnMain([this, alive, rawHost, code] {
            if (!alive.expired() && host_.get() == rawHost) {
                hostExited(code);
            }
        });
    };
    ExtensionHost::Settings settings;
    settings.hostScript = hostScriptPath();
    settings.workingDirectory = rootFolder.empty() ? homeFolder() : rootFolder;
    std::string error;
    if (!host->start(settings, &error)) {
        initializing_ = false;
        finishWaiting(false);
        delegate_.hostShowStatusMessage(error, 8);
        return;
    }
    host_ = std::move(host);
    std::string root = rootFolder.empty() ? homeFolder() : rootFolder;
    Json parameters = {
        {"extensions", extensionDescriptions()},
        {"workspaceFolders", Json::array({lsp::fileURI(root)})},
        {"configuration", configuration()},
        {"storageRoot", utf8FromPath(Services::shared().supportFolder() / "extension-state")},
        {"appRoot", utf8FromPath(programFolder())},
        {"languageIds", Json::array()},
        {"languageForPath", languageByExtension()},
    };
    host_->request("initialize", parameters, [this, alive, rawHost](const Json&, const std::string& problem) {
        std::string failure = problem;
        runOnMain([this, alive, rawHost, failure] {
            if (alive.expired() || host_.get() != rawHost) {
                return;
            }
            initializing_ = false;
            initialized_ = failure.empty();
            finishWaiting(failure.empty());
            if (failure.empty()) {
                delegate_.hostProvidersChanged();
            }
        });
    });
}

void ExtensionHostService::finishWaiting(bool ok) {
    std::vector<std::function<void(bool)>> waiting;
    waiting.swap(waiting_);
    for (auto& done : waiting) {
        done(ok);
    }
}

void ExtensionHostService::hostExited(int code) {
    bool wasStopping = stopping_;
    host_.reset();
    initialized_ = false;
    initializing_ = false;
    activatedLanguages_.clear();
    lastEditorState_.clear();
    providerLanguages_.clear();
    providersForAnyLanguage_ = false;
    registeredCommands_.clear();
    statusItems_.clear();
    finishWaiting(false);
    delegate_.hostStatusItemsChanged();
    if (wasStopping) {
        return;
    }
    ++restarts_;
    appendLog("The extension host stopped (exit code " + std::to_string(code) + ").\n", "Extension Host");
    if (restarts_ < kMostRestarts) {
        delegate_.hostShowStatusMessage("The extension host stopped; restarting…", 4);
        delegate_.hostProvidersChanged();
    } else {
        delegate_.hostShowStatusMessage("The extension host keeps stopping; extensions are off for this window. See the Extension Host log.", 10);
    }
}

void ExtensionHostService::startIfExtensionsWantToRun() {
    buildIndexIfNeeded();
    bool wanted = false;
    for (const IndexEntry& entry : index_) {
        wanted = wanted || entry.runsAtStartup;
    }
    if (!wanted) {
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    ensureHost([this, alive](bool ok) {
        if (!ok || alive.expired() || !host_) {
            return;
        }
        host_->request("host/event", Json{{"name", "onStartupFinished"}}, [](const Json&, const std::string&) {});
    });
}

void ExtensionHostService::shutdown() {
    stopping_ = true;
    if (host_) {
        host_->stop();
    }
    host_.reset();
    initialized_ = false;
    initializing_ = false;
    activatedLanguages_.clear();
    finishWaiting(false);
}

void ExtensionHostService::restart() {
    shutdown();
    restarts_ = 0;
    indexBuilt_ = false;
    statusItems_.clear();
    delegate_.hostStatusItemsChanged();
    delegate_.hostProvidersChanged();
    startIfExtensionsWantToRun();
}

void ExtensionHostService::setEditorState(const Json& state) {
    std::string text = state.dump();
    if (!initialized_ || !host_ || text == lastEditorState_) {
        return;
    }
    lastEditorState_ = text;
    host_->notify("editor/state", state);
}

void ExtensionHostService::filesDidChange(const std::vector<std::string>& paths) {
    if (!initialized_ || !host_) {
        return;
    }
    Json events = Json::array();
    for (const std::string& path : paths) {
        if (path.find("/.git/") != std::string::npos || path.find("\\.git\\") != std::string::npos || events.size() >= 5000) {
            continue;
        }
        std::error_code error;
        int type = fs::exists(pathFromUtf8(path), error) ? 2 : 3;
        events.push_back({{"uri", lsp::fileURI(path)}, {"type", type}});
    }
    if (!events.empty()) {
        host_->notify("workspace/fileEvents", Json{{"events", events}});
    }
}

void ExtensionHostService::settingsDidChange() {
    if (initialized_ && host_) {
        host_->notify("workspace/didChangeConfiguration", Json{{"configuration", configuration()}});
    }
}

// ---------------------------------------------------------------------------
// Language features

bool ExtensionHostService::extensionsMayServeLanguage(const std::string& language) {
    buildIndexIfNeeded();
    if (initialized_ || providersForAnyLanguage_ || providerLanguages_.count(language) > 0) {
        return true;
    }
    for (const IndexEntry& entry : index_) {
        if (entry.languages.count(language) > 0 || entry.activationEvents.count("onLanguage:" + language) > 0) {
            return true;
        }
    }
    return false;
}

void ExtensionHostService::prepareLanguage(const std::string& language, std::function<void(bool)> done) {
    std::weak_ptr<bool> alive = alive_;
    ensureHost([this, alive, language, done](bool ok) {
        if (!ok || alive.expired() || !host_) {
            done(false);
            return;
        }
        if (activatedLanguages_.count(language) > 0) {
            done(true);
            return;
        }
        ExtensionHost* rawHost = host_.get();
        host_->request("host/event", Json{{"name", "onLanguage:" + language}}, [this, alive, rawHost, language, done](const Json&, const std::string&) {
            runOnMain([this, alive, rawHost, language, done] {
                if (alive.expired() || host_.get() != rawHost) {
                    done(false);
                    return;
                }
                activatedLanguages_.insert(language);
                done(true);
            });
        });
    });
}

std::unique_ptr<lsp::ServerTransport> ExtensionHostService::newTransportForLanguage(const std::string& language, const std::string& rootUri) {
    if (!initialized_ || !host_ || !host_->isRunning()) {
        std::weak_ptr<bool> alive = alive_;
        ensureHost([this, alive](bool ok) {
            if (ok && !alive.expired()) {
                delegate_.hostProvidersChanged();
            }
        });
        return nullptr;
    }
    return host_->makeServerTransport(language, rootUri);
}

// ---------------------------------------------------------------------------
// Commands

std::vector<PaletteItem> ExtensionHostService::commandItems(const std::string& query) {
    buildIndexIfNeeded();
    std::map<std::string, std::string> titles;
    for (const ContributedCommand& command : contributedCommands_) {
        if (!command.hiddenFromPalette) {
            titles[command.identifier] = command.title;
        }
    }
    for (const auto& [identifier, title] : registeredCommands_) {
        if (!title.empty() && titles.count(identifier) == 0) {
            titles[identifier] = title;
        }
    }
    std::vector<PaletteItem> items;
    for (const auto& [identifier, title] : titles) {
        PaletteItem item;
        item.title = title;
        item.subtitle = "Extension";
        if (!paletteMatch(query, item, nullptr)) {
            continue;
        }
        std::string commandIdentifier = identifier;
        item.run = [this, commandIdentifier] { executeCommand(commandIdentifier, Json::array()); };
        items.push_back(std::move(item));
    }
    return items;
}

void ExtensionHostService::executeCommand(const std::string& identifier, const Json& arguments) {
    std::weak_ptr<bool> alive = alive_;
    ensureHost([this, alive, identifier, arguments](bool ok) {
        if (alive.expired()) {
            return;
        }
        if (!ok || !host_) {
            delegate_.hostShowStatusMessage("Extensions aren't running.", 4);
            return;
        }
        Json args = arguments.is_array() ? arguments : (arguments.is_null() ? Json::array() : Json::array({arguments}));
        host_->request("commands/execute", {{"id", identifier}, {"args", args}}, [this, alive, identifier](const Json&, const std::string& problem) {
            if (problem.empty()) {
                return;
            }
            std::string failure = problem;
            runOnMain([this, alive, identifier, failure] {
                if (!alive.expired()) {
                    delegate_.hostShowStatusMessage(identifier + " failed: " + failure, 8);
                }
            });
        });
    });
}

// ---------------------------------------------------------------------------
// Status and logs

std::vector<ExtensionStatusItem> ExtensionHostService::statusItems() const {
    std::vector<ExtensionStatusItem> items;
    for (const auto& [identifier, item] : statusItems_) {
        items.push_back(item);
    }
    std::sort(items.begin(), items.end(), [](const ExtensionStatusItem& a, const ExtensionStatusItem& b) {
        if (a.priority != b.priority) {
            return a.priority > b.priority;
        }
        return a.identifier < b.identifier;
    });
    return items;
}

std::string ExtensionHostService::summary() const {
    size_t running = 0;
    for (const auto& [identifier, state] : extensionStates_) {
        running += state == "active" ? 1 : 0;
    }
    if (running == 0) {
        return std::string();
    }
    return running == 1 ? "1 extension running" : std::to_string(running) + " extensions running";
}

std::string ExtensionHostService::logPathForChannel(const std::string& channel) {
    std::string safe;
    for (char c : channel) {
        safe += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.') ? c : '_';
    }
    fs::path folder = Services::shared().extensionLogsFolder();
    std::error_code error;
    fs::create_directories(folder, error);
    return utf8FromPath(folder / pathFromUtf8(safe + ".log"));
}

std::vector<std::string> ExtensionHostService::logChannels() const {
    return std::vector<std::string>(knownChannels_.begin(), knownChannels_.end());
}

void ExtensionHostService::appendLog(const std::string& text, const std::string& channel) {
    knownChannels_.insert(channel);
    bool firstWrite = openedChannels_.insert(channel).second;
    std::string path = logPathForChannel(channel);
    // Output channels are plain text files, so they can be opened and searched like any other file.
    std::ofstream out(pathFromUtf8(path), firstWrite ? std::ios::binary | std::ios::trunc : std::ios::binary | std::ios::app);
    out << text;
}

// ---------------------------------------------------------------------------
// Messages from the host

void ExtensionHostService::handleNotification(const std::string& method, const Json& incoming) {
    Json params = incoming.is_object() ? incoming : Json::object();
    if (method == "ui/log") {
        std::string channel = stringOf(params["channel"]);
        appendLog(stringOf(params["text"]), channel.empty() ? "Extension Host" : channel);
    } else if (method == "ui/logClear") {
        std::string channel = stringOf(params["channel"]);
        std::ofstream out(pathFromUtf8(logPathForChannel(channel.empty() ? "Extension Host" : channel)), std::ios::trunc);
    } else if (method == "ui/logShow") {
        std::string channel = stringOf(params["channel"]);
        delegate_.hostOpenFile(logPathForChannel(channel.empty() ? "Extension Host" : channel), -1, 0);
    } else if (method == "ui/statusBar") {
        std::string identifier = stringOf(params["id"]);
        if (identifier.empty()) {
            return;
        }
        if (!params.value("visible", false)) {
            statusItems_.erase(identifier);
        } else {
            ExtensionStatusItem& item = statusItems_[identifier];
            item.identifier = identifier;
            item.text = withoutIconMarkup(stringOf(params["text"]));
            item.tooltip = stringOf(params["tooltip"]);
            item.command = stringOf(params["command"]);
            item.alignment = params.value("alignment", 1);
            item.priority = params.value("priority", 0.0);
        }
        delegate_.hostStatusItemsChanged();
    } else if (method == "ui/statusMessage") {
        delegate_.hostShowStatusMessage(withoutIconMarkup(stringOf(params["text"])), params.value("timeout", 0) / 1000.0);
    } else if (method == "commands/registered") {
        std::string identifier = stringOf(params["id"]);
        if (!identifier.empty()) {
            registeredCommands_[identifier] = stringOf(params["title"]);
        }
    } else if (method == "commands/unregistered") {
        registeredCommands_.erase(stringOf(params["id"]));
    } else if (method == "host/extensionState") {
        std::string identifier = stringOf(params["id"]);
        std::string state = stringOf(params["state"]);
        if (!identifier.empty() && !state.empty()) {
            extensionStates_[identifier] = state;
        }
        if (state == "failed") {
            delegate_.hostShowStatusMessage(identifier + " couldn't start. See the Extension Host log.", 8);
        }
    } else if (method == "commands/setContext") {
        std::string key = stringOf(params["key"]);
        const Json& value = params.contains("value") ? params["value"] : Json();
        std::string text = value.is_string() ? value.get<std::string>() : value.is_null() ? std::string() : value.dump();
        if (!key.empty()) {
            delegate_.hostSetContextKey(key, text);
        }
    } else if (method == "editor/setSelections") {
        delegate_.hostSetSelections(params.value("selections", Json::array()), lsp::pathFromURI(params.value("uri", "")));
    } else if (method == "editor/reveal") {
        delegate_.hostRevealRange(params.value("range", Json::object()), lsp::pathFromURI(params.value("uri", "")));
    } else if (method == "editor/decorations") {
        delegate_.hostSetDecorations(lsp::pathFromURI(params.value("uri", "")), stringOf(params["key"]), params.value("options", Json::object()),
                                     params.value("ranges", Json::array()));
    } else if (method == "providers/changed") {
        providerLanguages_.clear();
        for (const Json& language : params.value("languages", Json::array())) {
            if (language.is_string()) {
                providerLanguages_.insert(language.get<std::string>());
            }
        }
        providersForAnyLanguage_ = params.value("any", false);
        delegate_.hostProvidersChanged();
    }
}

void ExtensionHostService::handleRequest(const std::string& method, const Json& incoming, ExtensionHost::Reply reply) {
    Json params = incoming.is_object() ? incoming : Json::object();
    if (method.rfind("webview/", 0) == 0) {
        reply(Json{{"delivered", false}}, "");  // Glasswork Bitmap shows no webviews
    } else if (method == "ui/showMessage") {
        showMessage(params, reply);
    } else if (method == "ui/quickPick") {
        showQuickPick(params, reply);
    } else if (method == "ui/inputBox") {
        showInputBox(params, reply);
    } else if (method == "workspace/applyEdit") {
        reply(Json{{"applied", delegate_.hostApplyWorkspaceEdit(params.value("edit", Json::object()))}}, "");
    } else if (method == "workspace/saveDocument") {
        reply(Json(delegate_.hostSaveFile(lsp::pathFromURI(params.value("uri", "")))), "");
    } else if (method == "workspace/saveAll") {
        delegate_.hostSaveAll();
        reply(Json(true), "");
    } else if (method == "workspace/updateConfiguration") {
        storeSetting(params);
        reply(Json(), "");
    } else if (method == "editor/insertSnippet") {
        bool inserted = delegate_.hostInsertSnippet(stringOf(params["snippet"]), params.value("ranges", Json::array()),
                                                    lsp::pathFromURI(params.value("uri", "")));
        reply(Json(inserted), "");
    } else if (method == "ui/openDocument") {
        long line = params.contains("line") && params["line"].is_number() ? params["line"].get<long>() : -1;
        long character = params.contains("character") && params["character"].is_number() ? params["character"].get<long>() : 0;
        delegate_.hostOpenFile(lsp::pathFromURI(params.value("uri", "")), line, character);
        reply(Json(), "");
    } else if (method == "env/openExternal") {
        std::string uri = stringOf(params["uri"]);
        if (!uri.empty()) {
            openExternally(uri);
        }
        reply(Json(!uri.empty()), "");
    } else if (method == "env/clipboardRead") {
        reply(Json(clipboardText()), "");
    } else if (method == "env/clipboardWrite") {
        setClipboardText(stringOf(params["text"]));
        reply(Json(), "");
    } else if (method == "commands/executeBuiltin") {
        bool handled = delegate_.hostRunBuiltinCommand(stringOf(params["id"]), params.value("args", Json::array()));
        reply(Json(), handled ? "" : "Unknown command");
    } else {
        reply(Json(), "Not supported: " + method);
    }
}

void ExtensionHostService::storeSetting(const Json& params) {
    // Settings an extension changes go next to settings.json, which keeps its comments.
    fs::path file = Services::shared().supportFolder() / "extension-settings.json";
    Json settings = Json::object();
    std::ifstream input(file, std::ios::binary);
    if (input) {
        std::stringstream text;
        text << input.rdbuf();
        Json parsed = parseJsonc(text.str());
        if (parsed.is_object()) {
            settings = parsed;
        }
    }
    input.close();
    std::string key = params.value("key", "");
    if (params.contains("value") && !params["value"].is_null()) {
        settings[key] = params["value"];
    } else {
        settings.erase(key);
    }
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << settings.dump(2) << "\n";
    output.close();
    settingsDidChange();
}

void ExtensionHostService::showMessage(const Json& params, ExtensionHost::Reply reply) {
    std::string severity = params.value("severity", "info");
    std::string message = stringOf(params["message"]);
    std::vector<std::string> items;
    for (const Json& item : params.value("items", Json::array())) {
        if (item.is_string()) {
            items.push_back(item.get<std::string>());
        }
    }
    bool modal = params.value("modal", false);
    // A plain notice with nothing to choose goes to the status line instead of in the way.
    if (items.empty() && !modal && severity == "info") {
        delegate_.hostShowStatusMessage(message, 6);
        reply(Json(), "");
        return;
    }
    DialogOverlay::Options options;
    options.title = severity == "error" ? "Error" : severity == "warning" ? "Warning" : "Message";
    options.lines.push_back(message);
    if (params.contains("detail") && params["detail"].is_string()) {
        options.lines.push_back("");
        options.lines.push_back(params["detail"].get<std::string>());
    }
    options.buttons = items;
    options.buttons.push_back(items.empty() ? "OK" : "Dismiss");
    delegate_.hostShowDialog(options, [items, reply](int button, const std::string&) {
        if (button >= 0 && static_cast<size_t>(button) < items.size()) {
            reply(Json(items[static_cast<size_t>(button)]), "");
        } else {
            reply(Json(), "");
        }
    });
}

void ExtensionHostService::showQuickPick(const Json& params, ExtensionHost::Reply reply) {
    struct Entry {
        std::string label;
        std::string subtitle;
        int index;
    };
    auto entries = std::make_shared<std::vector<Entry>>();
    for (const Json& entry : params.value("items", Json::array())) {
        if (!entry.is_object() || entry.value("kind", 0) == -1) {
            continue;  // a separator
        }
        std::string description = stringOf(entry.value("description", Json()));
        std::string detail = stringOf(entry.value("detail", Json()));
        entries->push_back({stringOf(entry.value("label", Json())), description.empty() ? detail : description, entry.value("index", 0)});
    }
    if (entries->empty()) {
        reply(Json(), "");
        return;
    }
    std::string placeholder = stringOf(params.value("placeHolder", Json()));
    if (placeholder.empty()) {
        placeholder = stringOf(params.value("title", Json()));
    }
    if (placeholder.empty()) {
        placeholder = "Select an item…";
    }
    auto answered = std::make_shared<bool>(false);
    delegate_.hostShowPalette(
        placeholder,
        [entries, answered, reply](const std::string& query) {
            std::vector<PaletteItem> items;
            for (const Entry& entry : *entries) {
                PaletteItem item;
                item.title = entry.label;
                item.subtitle = entry.subtitle;
                if (!paletteMatch(query, item, nullptr)) {
                    continue;
                }
                int index = entry.index;
                item.run = [answered, reply, index] {
                    *answered = true;
                    reply(Json(index), "");
                };
                items.push_back(std::move(item));
            }
            return items;
        },
        [answered, reply] {
            if (!*answered) {
                *answered = true;
                reply(Json(), "");
            }
        });
}

void ExtensionHostService::showInputBox(const Json& params, ExtensionHost::Reply reply) {
    DialogOverlay::Options options;
    std::string prompt = stringOf(params.value("prompt", Json()));
    std::string title = stringOf(params.value("title", Json()));
    options.title = title.empty() ? "Input" : title;
    options.lines.push_back(prompt.empty() ? "Enter a value" : prompt);
    options.buttons = {"OK", "Cancel"};
    options.hasField = true;
    options.fieldText = stringOf(params.value("value", Json()));
    options.placeholder = stringOf(params.value("placeHolder", Json()));
    delegate_.hostShowDialog(options, [reply](int button, const std::string& text) {
        if (button == 0) {
            reply(Json(text), "");
        } else {
            reply(Json(), "");
        }
    });
}

}  // namespace gw::app
