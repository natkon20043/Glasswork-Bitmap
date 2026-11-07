#include "LanguageService.hpp"

#include "Loop.hpp"
#include "Paths.hpp"
#include "Services.hpp"
#include "SettingsLookup.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace gw::app {

namespace {

/// Files larger than this are not sent to language servers.
constexpr size_t kLargestTrackedFile = 4 * 1024 * 1024;
/// How long after the last keystroke the servers hear about the edit.
constexpr double kSyncDelay = 0.25;

bool isMergedMethod(const std::string& method) {
    static const std::set<std::string> merged = {
        "textDocument/completion",     "textDocument/hover",           "textDocument/codeAction",
        "textDocument/definition",     "textDocument/declaration",     "textDocument/typeDefinition",
        "textDocument/implementation", "textDocument/references",      "textDocument/documentHighlight",
        "textDocument/codeLens",       "textDocument/documentLink",    "textDocument/inlayHint",
        "textDocument/foldingRange",   "textDocument/documentSymbol",  "workspace/symbol",
    };
    return merged.count(method) > 0;
}

bool prefersExtensions(const std::string& method) {
    return method == "textDocument/formatting" || method == "textDocument/rangeFormatting" ||
           method == "textDocument/onTypeFormatting";
}

void collectHoverText(const lsp::Json& contents, std::vector<std::string>& into) {
    if (contents.is_string()) {
        into.push_back(contents.get<std::string>());
    } else if (contents.is_array()) {
        for (const lsp::Json& piece : contents) {
            collectHoverText(piece, into);
        }
    } else if (contents.is_object() && contents.contains("language") && contents.contains("value")) {
        into.push_back("```" + contents.value("language", "") + "\n" + contents.value("value", "") + "\n```");
    } else if (contents.is_object() && contents.contains("value") && contents["value"].is_string()) {
        into.push_back(contents["value"].get<std::string>());
    }
}

lsp::Json mergeAnswers(const std::string& method, const std::vector<lsp::Json>& answers) {
    using Json = lsp::Json;
    if (method == "textDocument/completion") {
        Json items = Json::array();
        bool incomplete = false;
        for (const Json& answer : answers) {
            if (answer.is_array()) {
                for (const Json& item : answer) {
                    items.push_back(item);
                }
            } else if (answer.is_object()) {
                incomplete = incomplete || answer.value("isIncomplete", false);
                for (const Json& item : answer.value("items", Json::array())) {
                    items.push_back(item);
                }
            }
        }
        return {{"isIncomplete", incomplete}, {"items", items}};
    }
    if (method == "textDocument/hover") {
        std::vector<std::string> pieces;
        Json range;
        for (const Json& answer : answers) {
            if (!answer.is_object() || !answer.contains("contents")) {
                continue;
            }
            collectHoverText(answer["contents"], pieces);
            if (range.is_null() && answer.contains("range")) {
                range = answer["range"];
            }
        }
        if (pieces.empty()) {
            return Json();
        }
        std::string joined;
        for (size_t index = 0; index < pieces.size(); ++index) {
            if (index > 0) {
                joined += "\n\n---\n\n";
            }
            joined += pieces[index];
        }
        Json result = {{"contents", {{"kind", "markdown"}, {"value", joined}}}};
        if (!range.is_null()) {
            result["range"] = range;
        }
        return result;
    }
    Json list = Json::array();
    for (const Json& answer : answers) {
        if (answer.is_array()) {
            for (const Json& item : answer) {
                list.push_back(item);
            }
        } else if (answer.is_object()) {
            list.push_back(answer);
        }
    }
    return list;
}

void tagAnswer(const std::string& method, lsp::Json& answer, const std::string& serverName) {
    lsp::Json* items = nullptr;
    if (answer.is_array()) {
        items = &answer;
    } else if (answer.is_object() && answer.contains("items") && answer["items"].is_array()) {
        items = &answer["items"];
    }
    bool wantsTag = method == "textDocument/completion" || method == "textDocument/codeAction" || method == "textDocument/codeLens";
    if (items == nullptr || !wantsTag) {
        return;
    }
    for (lsp::Json& item : *items) {
        if (item.is_object()) {
            item["__gwServer"] = serverName;
        }
    }
}

std::string extensionServerName(const std::string& languageId) {
    return "extensions:" + languageId;
}

}  // namespace

LanguageService::LanguageService() : specs_(lsp::defaultServerSpecs()) {}

LanguageService::~LanguageService() {
    shutdown();
}

LanguageService::Attachment* LanguageService::attachmentNamed(Tracked& tracked, const std::string& name) {
    for (Attachment& attachment : tracked.attachments) {
        if (attachment.serverName == name) {
            return &attachment;
        }
    }
    return nullptr;
}

bool LanguageService::extensionsServeLanguage(const std::string& languageId) {
    return extensionProvider != nullptr && extensionProvider->extensionsMayServeLanguage(languageId);
}

bool LanguageService::hasServerForLanguage(const std::string& languageId) {
    return lsp::serverForLanguage(specs_, languageId) != nullptr || extensionsServeLanguage(languageId);
}

std::string LanguageService::rootFolderOrHome() const {
    return rootFolder.empty() ? homeFolder() : rootFolder;
}

void LanguageService::wireServer(lsp::LanguageServer* server, const std::string& name) {
    std::weak_ptr<bool> alive = alive_;
    server->onDiagnostics = [this, alive, name](const std::string& uri, long, std::vector<lsp::Diagnostic> diagnostics) {
        auto shared = std::make_shared<std::vector<lsp::Diagnostic>>(std::move(diagnostics));
        std::string copiedUri = uri;
        runOnMain([this, alive, name, shared, copiedUri] {
            if (alive.expired()) {
                return;
            }
            try {
                receivedDiagnostics(*shared, copiedUri, name);
            } catch (const std::exception& problem) {
                std::fprintf(stderr, "Glasswork Bitmap: couldn't use the problems %s reported: %s\n", name.c_str(), problem.what());
            }
        });
    };
    server->onApplyEdit = [this, alive](const lsp::Json& edit, std::function<void(bool)> reply) {
        auto sharedEdit = std::make_shared<lsp::Json>(edit);
        runOnMain([this, alive, sharedEdit, reply] {
            if (alive.expired() || !onApplyEdit) {
                reply(false);
                return;
            }
            try {
                onApplyEdit(*sharedEdit, reply);
            } catch (const std::exception&) {
                reply(false);
            }
        });
    };
    server->onShowMessage = [this, alive, name](int type, const std::string& message) {
        std::string text = message;
        runOnMain([this, alive, name, type, text] {
            if (!alive.expired() && onServerMessage) {
                onServerMessage(name, type, text);
            }
        });
    };
    server->onLogMessage = [this, alive, name](int, const std::string& message) {
        std::string text = message;
        runOnMain([this, alive, name, text] {
            if (!alive.expired() && onServerLog) {
                onServerLog(name, text);
            }
        });
    };
    server->onProgress = [this, alive, name](const std::string& token, const std::string& kind, const std::string& title,
                                             const std::string& message, int percentage) {
        std::string tokenText = token, kindText = kind, titleText = title, messageText = message;
        runOnMain([=, this] {
            if (!alive.expired() && onServerProgress) {
                onServerProgress(name, tokenText, kindText, titleText, messageText, percentage);
            }
        });
    };
    server->onShowMessageRequest = [this, alive, name](int, const std::string& message, const std::vector<std::string>& actions,
                                                       std::function<void(const std::string&)> reply) {
        std::string text = message;
        std::vector<std::string> titles = actions;
        runOnMain([this, alive, name, text, titles, reply] {
            if (alive.expired() || !onServerQuestion) {
                reply("");
                return;
            }
            onServerQuestion(name, text, titles, reply);
        });
    };
    std::shared_ptr<SettingsSnapshot> snapshot = settings_;
    server->onConfiguration = [snapshot](const std::string& section, const std::string&) -> lsp::Json {
        std::lock_guard<std::mutex> lock(snapshot->mutex);
        return settingsSection(snapshot->values, section);
    };
    server->onReady = [this, alive] {
        runOnMain([this, alive] {
            if (!alive.expired() && onStatusChanged) {
                onStatusChanged();
            }
        });
    };
    server->onExit = [this, alive, name, server](int) {
        runOnMain([this, alive, name, server] {
            if (!alive.expired()) {
                serverEnded(name, server);
            }
        });
    };
}

lsp::LanguageServer* LanguageService::serverForSpec(const lsp::ServerSpec& spec) {
    ServerEntry& entry = servers_[spec.name];
    if (entry.server != nullptr && entry.server->isRunning()) {
        return entry.server.get();
    }
    if (entry.failedToStart) {
        return nullptr;
    }
    entry.openUris.clear();
    entry.server = std::make_unique<lsp::LanguageServer>(spec, rootFolderOrHome());
    wireServer(entry.server.get(), spec.name);
    std::string error;
    if (!entry.server->start(&error)) {
        entry.failedToStart = true;
        entry.failureMessage = error;
        entry.server.reset();
        if (onStatusChanged) {
            onStatusChanged();
        }
        return nullptr;
    }
    if (onStatusChanged) {
        onStatusChanged();
    }
    return entry.server.get();
}

lsp::LanguageServer* LanguageService::extensionServerForLanguage(const std::string& languageId) {
    std::string name = extensionServerName(languageId);
    ServerEntry& entry = servers_[name];
    if (entry.server != nullptr && entry.server->isRunning()) {
        return entry.server.get();
    }
    if (extensionProvider == nullptr) {
        return nullptr;
    }
    std::string root = rootFolderOrHome();
    std::unique_ptr<lsp::ServerTransport> transport = extensionProvider->newTransportForLanguage(languageId, lsp::fileURI(root));
    if (transport == nullptr) {
        return nullptr;
    }
    lsp::ServerSpec spec;
    spec.name = name;
    entry.openUris.clear();
    entry.fromExtensions = true;
    entry.server = std::make_unique<lsp::LanguageServer>(spec, root, std::move(transport));
    wireServer(entry.server.get(), name);
    std::string error;
    if (!entry.server->start(&error)) {
        entry.server.reset();
        return nullptr;
    }
    if (onStatusChanged) {
        onStatusChanged();
    }
    return entry.server.get();
}

void LanguageService::serverEnded(const std::string& name, lsp::LanguageServer* instance) {
    auto found = servers_.find(name);
    if (found == servers_.end() || found->second.server.get() != instance) {
        return;
    }
    found->second.openUris.clear();
    for (auto& [document, tracked] : tracked_) {
        for (Attachment& attachment : tracked.attachments) {
            if (attachment.serverName == name) {
                attachment.sentVersion = 0;
            }
        }
    }
    if (onStatusChanged) {
        onStatusChanged();
    }
}

void LanguageService::filesDidChange(const std::vector<std::string>& paths) {
    std::vector<lsp::LanguageServer::FileEvent> events;
    for (const std::string& path : paths) {
        if (path.find("/.git/") != std::string::npos || path.find("\\.git\\") != std::string::npos || events.size() >= 5000) {
            continue;
        }
        lsp::LanguageServer::FileEvent event;
        event.path = path;
        std::error_code error;
        event.type = std::filesystem::exists(pathFromUtf8(path), error) ? 2 : 3;
        events.push_back(std::move(event));
    }
    if (events.empty()) {
        return;
    }
    for (auto& [name, entry] : servers_) {
        if (entry.server != nullptr && entry.server->isReady() && !entry.fromExtensions && entry.server->watchesFiles()) {
            entry.server->didChangeWatchedFiles(events);
        }
    }
}

void LanguageService::settingsDidChange(const Json& settings) {
    {
        std::lock_guard<std::mutex> lock(settings_->mutex);
        settings_->values = settings;
    }
    for (auto& [name, entry] : servers_) {
        if (entry.server != nullptr && entry.server->isReady() && !entry.fromExtensions) {
            entry.server->didChangeConfiguration(settings);
        }
    }
}

void LanguageService::shutdown() {
    if (flushTimer_ != 0) {
        cancelTimer(flushTimer_);
        flushTimer_ = 0;
    }
    if (inlayTimer_ != 0) {
        cancelTimer(inlayTimer_);
        inlayTimer_ = 0;
    }
    for (auto& [name, entry] : servers_) {
        if (entry.server != nullptr) {
            entry.server->stop();
        }
    }
    servers_.clear();
    tracked_.clear();
}

// ---------------------------------------------------------------------------
// Documents

void LanguageService::trackDocument(Document* document) {
    if (!enabled || document == nullptr || !document->path() || document->readOnly) {
        return;
    }
    const std::string& languageId = document->languageId;
    const lsp::ServerSpec* spec = lsp::serverForLanguage(specs_, languageId);
    bool extensionsWanted = extensionsServeLanguage(languageId);
    std::string uri = lsp::fileURI(utf8FromPath(*document->path()));
    auto existing = tracked_.find(document);
    if (existing != tracked_.end()) {
        Tracked& current = existing->second;
        bool hasNative = !current.attachments.empty() && !current.attachments.front().fromExtensions;
        bool sameNative = spec != nullptr ? (hasNative && current.attachments.front().serverName == spec->name) : !hasNative;
        if (sameNative && current.languageId == languageId && current.uri == uri) {
            if (extensionsWanted) {
                attachExtensions(document);
            }
            return;
        }
        untrackDocument(document);
    }
    if (spec == nullptr && !extensionsWanted) {
        return;
    }
    if (document->buffer().byteSize() > kLargestTrackedFile) {
        return;
    }
    Tracked tracked;
    tracked.uri = uri;
    tracked.languageId = languageId;
    document->recordSyncEdits(true);
    if (spec != nullptr) {
        if (lsp::LanguageServer* server = serverForSpec(*spec)) {
            server->didOpen(uri, languageId, document->version(), document->buffer().text());
            servers_[spec->name].openUris.insert(uri);
            Attachment attachment;
            attachment.serverName = spec->name;
            attachment.sentVersion = document->version();
            tracked.attachments.push_back(attachment);
        }
    }
    if (tracked.attachments.empty() && !extensionsWanted) {
        document->recordSyncEdits(false);
        return;
    }
    tracked_[document] = std::move(tracked);
    if (extensionsWanted) {
        attachExtensions(document);
    }
}

void LanguageService::attachExtensions(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return;
    }
    Tracked& tracked = found->second;
    if (tracked.extensionsRequested || extensionProvider == nullptr) {
        return;
    }
    std::string languageId = tracked.languageId;
    if (Attachment* attachment = attachmentNamed(tracked, extensionServerName(languageId))) {
        ServerEntry& entry = servers_[extensionServerName(languageId)];
        if (attachment != nullptr && entry.server != nullptr && entry.server->isRunning()) {
            return;
        }
    }
    tracked.extensionsRequested = true;
    std::string uriAtRequest = tracked.uri;
    std::weak_ptr<bool> alive = alive_;
    extensionProvider->prepareLanguage(languageId, [this, alive, document, uriAtRequest, languageId](bool ok) {
        if (alive.expired()) {
            return;
        }
        auto stillTracked = tracked_.find(document);
        if (stillTracked == tracked_.end() || stillTracked->second.uri != uriAtRequest) {
            return;
        }
        stillTracked->second.extensionsRequested = false;
        if (ok) {
            attachExtensionServer(languageId, document);
        }
    });
}

void LanguageService::attachExtensionServer(const std::string& languageId, Document* document) {
    if (tracked_.find(document) == tracked_.end()) {
        return;
    }
    lsp::LanguageServer* server = extensionServerForLanguage(languageId);
    if (server == nullptr) {
        return;
    }
    flushDocument(document);
    auto found = tracked_.find(document);
    Tracked& tracked = found->second;
    std::string name = extensionServerName(languageId);
    ServerEntry& entry = servers_[name];
    server->didOpen(tracked.uri, tracked.languageId, document->version(), document->buffer().text());
    entry.openUris.insert(tracked.uri);
    Attachment* attachment = attachmentNamed(tracked, name);
    if (attachment == nullptr) {
        Attachment fresh;
        fresh.serverName = name;
        fresh.fromExtensions = true;
        tracked.attachments.push_back(fresh);
        attachment = &tracked.attachments.back();
    }
    attachment->sentVersion = document->version();
    if (onStatusChanged) {
        onStatusChanged();
    }
    requestInlayHints(document);
}

void LanguageService::documentDidChange(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return;
    }
    found->second.pendingChange = true;
    if (flushTimer_ != 0) {
        cancelTimer(flushTimer_);
    }
    flushTimer_ = afterDelay(kSyncDelay, [this] {
        flushTimer_ = 0;
        flushAllPending();
    });
}

void LanguageService::flushAllPending() {
    std::vector<Document*> pending;
    for (auto& [document, tracked] : tracked_) {
        if (tracked.pendingChange) {
            pending.push_back(document);
        }
    }
    for (Document* document : pending) {
        flushDocument(document);
    }
}

void LanguageService::flushDocument(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return;
    }
    Tracked& tracked = found->second;
    tracked.pendingChange = false;
    uint64_t version = document->version();
    bool anyBehind = false;
    for (const Attachment& attachment : tracked.attachments) {
        if (attachment.sentVersion != version) {
            anyBehind = true;
        }
    }
    if (!anyBehind) {
        return;
    }
    std::optional<std::vector<SyncEdit>> edits = document->takeSyncEdits();
    std::string fullText;
    bool haveFullText = false;
    auto text = [&]() -> const std::string& {
        if (!haveFullText) {
            fullText = document->buffer().text();
            haveFullText = true;
        }
        return fullText;
    };
    for (Attachment& attachment : tracked.attachments) {
        if (attachment.sentVersion == version) {
            continue;
        }
        ServerEntry& entry = servers_[attachment.serverName];
        if (entry.server == nullptr || !entry.server->isRunning()) {
            lsp::LanguageServer* server = nullptr;
            if (attachment.fromExtensions) {
                server = extensionServerForLanguage(tracked.languageId);
            } else {
                const lsp::ServerSpec* spec = lsp::serverForLanguage(specs_, tracked.languageId);
                server = spec ? serverForSpec(*spec) : nullptr;
            }
            if (server == nullptr) {
                continue;
            }
            server->didOpen(tracked.uri, tracked.languageId, version, text());
            servers_[attachment.serverName].openUris.insert(tracked.uri);
            attachment.sentVersion = version;
            continue;
        }
        if (entry.openUris.count(tracked.uri) == 0) {
            entry.server->didOpen(tracked.uri, tracked.languageId, version, text());
            entry.openUris.insert(tracked.uri);
        } else {
            bool canSendEdits = entry.server->wantsIncrementalSync() && edits.has_value() && !edits->empty();
            if (canSendEdits) {
                entry.server->didChangeIncremental(tracked.uri, version, *edits);
            } else {
                entry.server->didChange(tracked.uri, version, text());
            }
        }
        attachment.sentVersion = version;
    }
    // Hints follow the text: ask again shortly after the servers have heard about the edit.
    if (inlayTimer_ != 0) {
        cancelTimer(inlayTimer_);
    }
    inlayTimer_ = afterDelay(0.6, [this, document] {
        inlayTimer_ = 0;
        if (tracked_.find(document) != tracked_.end()) {
            requestInlayHints(document);
        }
    });
}

void LanguageService::documentDidSave(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return;
    }
    flushDocument(document);
    found = tracked_.find(document);
    for (const Attachment& attachment : found->second.attachments) {
        ServerEntry& entry = servers_[attachment.serverName];
        if (entry.server != nullptr && entry.server->isRunning()) {
            entry.server->didSave(found->second.uri);
        }
    }
}

void LanguageService::closeDocumentOnServers(const Tracked& tracked) {
    for (const Attachment& attachment : tracked.attachments) {
        ServerEntry& entry = servers_[attachment.serverName];
        if (entry.server != nullptr && entry.server->isRunning() && entry.openUris.count(tracked.uri) > 0) {
            entry.server->didClose(tracked.uri);
        }
        entry.openUris.erase(tracked.uri);
    }
}

void LanguageService::untrackDocument(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return;
    }
    closeDocumentOnServers(found->second);
    tracked_.erase(found);
    document->recordSyncEdits(false);
    if (onDiagnosticsChanged) {
        onDiagnosticsChanged(document);
    }
}

void LanguageService::documentDidMove(Document* document) {
    auto found = tracked_.find(document);
    if (found != tracked_.end()) {
        closeDocumentOnServers(found->second);
        tracked_.erase(found);
        document->recordSyncEdits(false);
    }
    trackDocument(document);
}

// ---------------------------------------------------------------------------
// Diagnostics, semantic tokens, inlay hints

void LanguageService::receivedDiagnostics(const std::vector<lsp::Diagnostic>& diagnostics, const std::string& uri,
                                          const std::string& serverName) {
    std::string wanted = lsp::normalizedURI(uri);
    for (auto& [document, tracked] : tracked_) {
        Attachment* attachment = lsp::normalizedURI(tracked.uri) == wanted ? attachmentNamed(tracked, serverName) : nullptr;
        if (attachment == nullptr) {
            continue;
        }
        const TextBuffer& buffer = document->buffer();
        std::vector<Diagnostic> converted;
        for (const lsp::Diagnostic& diagnostic : diagnostics) {
            try {
                Diagnostic item;
                item.range.start = lsp::position(buffer, diagnostic.range.at("start"));
                item.range.end = lsp::position(buffer, diagnostic.range.at("end"));
                item.severity = diagnostic.severity;
                item.message = diagnostic.message;
                item.source = diagnostic.source;
                item.raw = std::make_shared<lsp::Json>(diagnostic.toJson());
                if (item.raw->contains("tags") && (*item.raw)["tags"].is_array()) {
                    for (const lsp::Json& tag : (*item.raw)["tags"]) {
                        if (tag.is_number_integer()) {
                            item.unnecessary = item.unnecessary || tag.get<int>() == 1;
                            item.deprecated = item.deprecated || tag.get<int>() == 2;
                        }
                    }
                }
                (*item.raw)["__gwServer"] = serverName;
                converted.push_back(std::move(item));
            } catch (const std::exception&) {
            }
        }
        attachment->diagnostics = std::move(converted);
        tracked.diagnostics.clear();
        for (const Attachment& each : tracked.attachments) {
            tracked.diagnostics.insert(tracked.diagnostics.end(), each.diagnostics.begin(), each.diagnostics.end());
        }
        Document* changed = document;
        if (onDiagnosticsChanged) {
            onDiagnosticsChanged(changed);
        }
        requestSemanticTokens(changed);
        requestInlayHints(changed);
        return;
    }
}

void LanguageService::requestSemanticTokens(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end() || !Services::shared().settings().semanticHighlighting) {
        return;
    }
    Tracked& tracked = found->second;
    uint64_t version = document->version();
    if (tracked.semanticRequestedVersion == version) {
        return;
    }
    lsp::LanguageServer* server = nullptr;
    for (const Attachment& attachment : tracked.attachments) {
        ServerEntry& candidate = servers_[attachment.serverName];
        if (candidate.server != nullptr && candidate.server->isReady() && candidate.server->supports("semanticTokensProvider")) {
            server = candidate.server.get();
            break;
        }
    }
    if (server == nullptr) {
        return;
    }
    lsp::Json capabilities = server->capabilities();
    const lsp::Json& provider = capabilities["semanticTokensProvider"];
    if (!provider.is_object() || !provider.contains("legend")) {
        return;
    }
    bool wantsFull = provider.contains("full") && (provider["full"].is_object() || provider["full"] == true);
    if (!wantsFull) {
        return;
    }
    tracked.semanticRequestedVersion = version;
    auto typeNames = std::make_shared<std::vector<std::string>>();
    auto modifierNames = std::make_shared<std::vector<std::string>>();
    for (const lsp::Json& name : provider["legend"].value("tokenTypes", lsp::Json::array())) {
        typeNames->push_back(name.is_string() ? name.get<std::string>() : std::string());
    }
    for (const lsp::Json& name : provider["legend"].value("tokenModifiers", lsp::Json::array())) {
        modifierNames->push_back(name.is_string() ? name.get<std::string>() : std::string());
    }
    std::weak_ptr<bool> alive = alive_;
    server->request("textDocument/semanticTokens/full", {{"textDocument", {{"uri", tracked.uri}}}},
                    [this, alive, document, version, typeNames, modifierNames](const lsp::Json& result, const std::string& error) {
        if (!error.empty() || !result.is_object() || !result["data"].is_array()) {
            return;
        }
        auto numbers = std::make_shared<std::vector<uint32_t>>();
        numbers->reserve(result["data"].size());
        for (const lsp::Json& number : result["data"]) {
            numbers->push_back(number.is_number_unsigned() ? number.get<uint32_t>() : 0);
        }
        runOnMain([this, alive, document, version, typeNames, modifierNames, numbers] {
            if (alive.expired()) {
                return;
            }
            auto entry = tracked_.find(document);
            if (entry == tracked_.end() || document->version() != version) {
                return;
            }
            auto set = std::make_shared<SemanticTokenSet>();
            set->tokens = decodeSemanticTokens(*numbers);
            set->typeNames = *typeNames;
            set->modifierNames = *modifierNames;
            entry->second.semantic = set;
            if (onSemanticTokensChanged) {
                onSemanticTokensChanged(document);
            }
        });
    });
}

void LanguageService::requestInlayHints(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return;
    }
    Tracked& tracked = found->second;
    if (!Services::shared().settings().inlayHints) {
        if (!tracked.inlayHints.empty()) {
            tracked.inlayHints.clear();
            if (onInlayHintsChanged) {
                onInlayHintsChanged(document);
            }
        }
        return;
    }
    uint64_t version = document->version();
    if (tracked.inlayRequestedVersion == version || !serverSupports("inlayHintProvider", document)) {
        return;
    }
    tracked.inlayRequestedVersion = version;
    const TextBuffer& buffer = document->buffer();
    lsp::Json params = {
        {"textDocument", {{"uri", tracked.uri}}},
        {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", lsp::position(buffer, buffer.end())}}},
    };
    request("textDocument/inlayHint", params, document, true, [this, document](const lsp::Json& result, const std::string& error) {
        auto stillTracked = tracked_.find(document);
        if (!error.empty() || !result.is_array() || stillTracked == tracked_.end()) {
            return;
        }
        std::vector<InlayHint> hints;
        const TextBuffer& current = document->buffer();
        for (const lsp::Json& item : result) {
            if (!item.is_object() || !item.contains("position") || !item.contains("label")) {
                continue;
            }
            InlayHint hint;
            try {
                hint.position = lsp::position(current, item["position"]);
            } catch (const std::exception&) {
                continue;
            }
            if (item["label"].is_string()) {
                hint.label = item["label"].get<std::string>();
            } else if (item["label"].is_array()) {
                for (const lsp::Json& part : item["label"]) {
                    hint.label += part.value("value", "");
                }
            }
            if (hint.label.empty()) {
                continue;
            }
            hint.isType = item.value("kind", 0) == 1;
            hint.padLeft = item.value("paddingLeft", false);
            hint.padRight = item.value("paddingRight", false);
            hints.push_back(std::move(hint));
            if (hints.size() >= 5000) {
                break;
            }
        }
        std::stable_sort(hints.begin(), hints.end(), [](const InlayHint& a, const InlayHint& b) { return a.position < b.position; });
        stillTracked->second.inlayHints = std::move(hints);
        if (onInlayHintsChanged) {
            onInlayHintsChanged(document);
        }
    });
}

const std::vector<InlayHint>& LanguageService::inlayHintsFor(Document* document) const {
    auto found = tracked_.find(document);
    return found == tracked_.end() ? noInlayHints_ : found->second.inlayHints;
}

std::shared_ptr<const SemanticTokenSet> LanguageService::semanticTokensFor(Document* document) const {
    auto found = tracked_.find(document);
    return found == tracked_.end() ? nullptr : found->second.semantic;
}

const std::vector<Diagnostic>& LanguageService::diagnosticsFor(Document* document) const {
    auto found = tracked_.find(document);
    return found == tracked_.end() ? noDiagnostics_ : found->second.diagnostics;
}

size_t LanguageService::errorCount() const {
    size_t total = 0;
    for (const auto& [document, tracked] : tracked_) {
        for (const Diagnostic& diagnostic : tracked.diagnostics) {
            total += diagnostic.severity == 1 ? 1 : 0;
        }
    }
    return total;
}

size_t LanguageService::warningCount() const {
    size_t total = 0;
    for (const auto& [document, tracked] : tracked_) {
        for (const Diagnostic& diagnostic : tracked.diagnostics) {
            total += diagnostic.severity == 2 ? 1 : 0;
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Status

std::string LanguageService::statusTextForLanguage(const std::string& languageId) {
    if (languageId.empty() || !enabled) {
        return std::string();
    }
    std::string text;
    if (const lsp::ServerSpec* spec = lsp::serverForLanguage(specs_, languageId)) {
        text = spec->name;
        auto found = servers_.find(spec->name);
        if (found != servers_.end()) {
            const ServerEntry& entry = found->second;
            if (entry.failedToStart) {
                text = spec->name + " isn't installed";
            } else if (entry.server == nullptr || !entry.server->isRunning()) {
                text = spec->name + " stopped";
            } else if (!entry.server->isReady()) {
                text = spec->name + " starting…";
            }
        }
    }
    if (extensionsServeLanguage(languageId)) {
        auto found = servers_.find(extensionServerName(languageId));
        bool running = found != servers_.end() && found->second.server != nullptr && found->second.server->isRunning();
        std::string extra = running ? "extensions" : "extensions starting…";
        text = text.empty() ? extra : text + " + " + extra;
    }
    return text;
}

std::string LanguageService::completionTriggerCharacters(Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return std::string();
    }
    std::string result;
    for (const Attachment& attachment : found->second.attachments) {
        ServerEntry& entry = servers_[attachment.serverName];
        if (entry.server == nullptr || !entry.server->isReady()) {
            continue;
        }
        lsp::Json capabilities = entry.server->capabilities();
        if (capabilities.contains("completionProvider") && capabilities["completionProvider"].is_object() &&
            capabilities["completionProvider"].contains("triggerCharacters")) {
            for (const lsp::Json& trigger : capabilities["completionProvider"]["triggerCharacters"]) {
                if (trigger.is_string() && result.find(trigger.get<std::string>()) == std::string::npos) {
                    result += trigger.get<std::string>();
                }
            }
        }
    }
    return result;
}

std::string LanguageService::installHintForLanguage(const std::string& languageId) {
    const lsp::ServerSpec* spec = lsp::serverForLanguage(specs_, languageId);
    if (spec == nullptr) {
        return std::string();
    }
    auto found = servers_.find(spec->name);
    if (found == servers_.end() || !found->second.failedToStart) {
        return std::string();
    }
    return found->second.failureMessage;
}

bool LanguageService::serverSupports(const std::string& capability, Document* document) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        return false;
    }
    for (const Attachment& attachment : found->second.attachments) {
        ServerEntry& entry = servers_[attachment.serverName];
        if (entry.server != nullptr && entry.server->isReady() && entry.server->supports(capability)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Requests

lsp::Json LanguageService::positionParams(Document* document, Position position) {
    auto found = tracked_.find(document);
    std::string uri = found != tracked_.end() ? found->second.uri
                      : document->path()          ? lsp::fileURI(utf8FromPath(*document->path()))
                                                  : std::string();
    return {{"textDocument", {{"uri", uri}}}, {"position", lsp::position(document->buffer(), position)}};
}

std::vector<std::string> LanguageService::serversForMethod(const std::string& method, const Json& params, Tracked& tracked) {
    if (params.is_object() && params.contains("__gwServer") && params["__gwServer"].is_string()) {
        std::string hint = params["__gwServer"].get<std::string>();
        if (attachmentNamed(tracked, hint) != nullptr) {
            return {hint};
        }
    }
    std::string key = lsp::capabilityKeyForMethod(method);
    if (method == "completionItem/resolve") {
        key = "completionProvider.resolveProvider";
    } else if (method == "codeAction/resolve") {
        key = "codeActionProvider.resolveProvider";
    } else if (method == "codeLens/resolve") {
        key = "codeLensProvider.resolveProvider";
    } else if (method == "textDocument/prepareRename") {
        key = "renameProvider";
    }
    std::vector<std::string> ordered;
    for (const Attachment& attachment : tracked.attachments) {
        ordered.push_back(attachment.serverName);
    }
    if (prefersExtensions(method)) {
        std::stable_partition(ordered.begin(), ordered.end(), [&](const std::string& name) {
            Attachment* attachment = attachmentNamed(tracked, name);
            return attachment != nullptr && attachment->fromExtensions;
        });
    }
    std::vector<std::string> capable;
    for (const std::string& name : ordered) {
        ServerEntry& entry = servers_[name];
        if (entry.server == nullptr || !entry.server->isRunning()) {
            continue;
        }
        if (key.empty() || entry.server->supports(key)) {
            capable.push_back(name);
        }
    }
    if (capable.empty() && !ordered.empty()) {
        capable.push_back(ordered.front());
    }
    if (!isMergedMethod(method) && capable.size() > 1) {
        capable.resize(1);
    }
    return capable;
}

void LanguageService::request(const std::string& method, Json params, Document* document, bool requireSameVersion,
                              Completion completion) {
    auto found = tracked_.find(document);
    if (found == tracked_.end()) {
        completion(Json(), "No language server is following this file.");
        return;
    }
    flushDocument(document);
    found = tracked_.find(document);
    std::vector<std::string> targets = serversForMethod(method, params, found->second);
    bool tagResults = targets.size() > 1 ||
                      (found->second.attachments.size() > 1 &&
                       (method == "textDocument/completion" || method == "textDocument/codeAction" || method == "textDocument/codeLens"));
    if (params.is_object()) {
        params.erase("__gwServer");
    }
    std::vector<lsp::LanguageServer*> servers;
    std::vector<std::string> names;
    for (const std::string& name : targets) {
        ServerEntry& entry = servers_[name];
        if (entry.server != nullptr && entry.server->isRunning()) {
            servers.push_back(entry.server.get());
            names.push_back(name);
        }
    }
    if (servers.empty()) {
        completion(Json(), "The language server isn't running.");
        return;
    }
    uint64_t versionAtRequest = document->version();
    struct Gathered {
        std::mutex mutex;
        size_t remaining = 0;
        std::vector<Json> answers;
        std::vector<std::string> errors;
    };
    auto gathered = std::make_shared<Gathered>();
    gathered->remaining = servers.size();
    gathered->answers.resize(servers.size());
    std::weak_ptr<bool> alive = alive_;
    for (size_t index = 0; index < servers.size(); ++index) {
        std::string serverName = names[index];
        servers[index]->request(method, params, [=, this](const Json& result, const std::string& error) {
            bool last = false;
            {
                std::lock_guard<std::mutex> lock(gathered->mutex);
                if (error.empty()) {
                    Json answer = result;
                    if (tagResults) {
                        tagAnswer(method, answer, serverName);
                    }
                    gathered->answers[index] = std::move(answer);
                } else {
                    gathered->errors.push_back(error);
                }
                last = --gathered->remaining == 0;
            }
            if (!last) {
                return;
            }
            Json combined;
            std::string firstError;
            {
                std::lock_guard<std::mutex> lock(gathered->mutex);
                bool anySucceeded = gathered->errors.size() < gathered->answers.size();
                if (!anySucceeded) {
                    firstError = gathered->errors.empty() ? std::string("Request failed") : gathered->errors.front();
                } else if (gathered->answers.size() == 1) {
                    combined = gathered->answers[0];
                } else {
                    combined = mergeAnswers(method, gathered->answers);
                }
            }
            auto sharedResult = std::make_shared<Json>(std::move(combined));
            runOnMain([=, this] {
                if (alive.expired() || tracked_.find(document) == tracked_.end()) {
                    return;
                }
                if (requireSameVersion && document->version() != versionAtRequest) {
                    return;
                }
                try {
                    completion(*sharedResult, firstError);
                } catch (const std::exception& problem) {
                    std::fprintf(stderr, "Glasswork Bitmap: couldn't use the reply to %s: %s\n", method.c_str(), problem.what());
                }
            });
        });
    }
}

}  // namespace gw::app
