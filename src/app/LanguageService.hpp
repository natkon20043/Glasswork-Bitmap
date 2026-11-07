#pragma once

#include "Document.hpp"
#include "EditorView.hpp"
#include "LspServer.hpp"
#include "SemanticTokens.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace gw::app {

/// How VS Code extensions take part in language features: per language, whether they have anything
/// to say, and a transport for a virtual server answering from their providers.
class ExtensionLanguageProvider {
public:
    virtual ~ExtensionLanguageProvider() = default;
    virtual bool extensionsMayServeLanguage(const std::string& languageId) = 0;
    /// Starts the host if needed; `done` runs on the main thread with whether there is a host to ask.
    virtual void prepareLanguage(const std::string& languageId, std::function<void(bool ok)> done) = 0;
    virtual std::unique_ptr<lsp::ServerTransport> newTransportForLanguage(const std::string& languageId, const std::string& rootUri) = 0;
};

/// Runs language servers (clangd, pyright, rust-analyzer...) for open files, keeps them in step with
/// the editor and collects what they report. A server starts when a file of its language first opens
/// and stops with the window. Main thread only; answers come back on the main thread.
class LanguageService {
public:
    using Json = lsp::Json;
    using Completion = std::function<void(const Json& result, const std::string& error)>;

    LanguageService();
    ~LanguageService();

    // ---- Lifecycle
    void shutdown();
    /// Paths changed on disk; servers that asked about such files are told.
    void filesDidChange(const std::vector<std::string>& paths);
    /// The user's settings, as servers see them (`workspace/configuration`).
    void settingsDidChange(const Json& settings);

    // ---- Documents
    void trackDocument(Document* document);
    void documentDidChange(Document* document);
    void documentDidSave(Document* document);
    void untrackDocument(Document* document);
    void documentDidMove(Document* document);

    // ---- Requests
    /// Pending changes are sent first. With `requireSameVersion` a reply for an edited document is dropped.
    void request(const std::string& method, Json params, Document* document, bool requireSameVersion, Completion completion);
    Json positionParams(Document* document, Position position);

    // ---- Results
    const std::vector<Diagnostic>& diagnosticsFor(Document* document) const;
    const std::vector<InlayHint>& inlayHintsFor(Document* document) const;
    std::shared_ptr<const SemanticTokenSet> semanticTokensFor(Document* document) const;
    size_t errorCount() const;
    size_t warningCount() const;

    // ---- Servers
    bool hasServerForLanguage(const std::string& languageId);
    bool serverSupports(const std::string& capability, Document* document);
    std::string completionTriggerCharacters(Document* document);
    std::string statusTextForLanguage(const std::string& languageId);
    std::string installHintForLanguage(const std::string& languageId);

    // ---- Properties
    std::string rootFolder;
    ExtensionLanguageProvider* extensionProvider = nullptr;
    bool enabled = true;

    // ---- Callbacks (main thread)
    std::function<void(Document*)> onDiagnosticsChanged;
    std::function<void(Document*)> onSemanticTokensChanged;
    std::function<void(Document*)> onInlayHintsChanged;
    std::function<void(const std::string& server, int type, const std::string& message)> onServerMessage;
    std::function<void(const std::string& server, const std::string& message)> onServerLog;
    std::function<void(const std::string& server, const std::string& token, const std::string& kind, const std::string& title,
                       const std::string& message, int percentage)>
        onServerProgress;
    std::function<void(const std::string& server, const std::string& message, const std::vector<std::string>& actions,
                       std::function<void(const std::string& chosen)> reply)>
        onServerQuestion;
    std::function<void(const Json& edit, std::function<void(bool applied)> reply)> onApplyEdit;
    std::function<void()> onStatusChanged;

private:
    struct Attachment {
        std::string serverName;
        bool fromExtensions = false;
        uint64_t sentVersion = 0;
        std::vector<Diagnostic> diagnostics;
    };
    struct Tracked {
        static constexpr uint64_t kNeverAsked = UINT64_MAX;
        std::string uri;
        std::string languageId;
        std::vector<Attachment> attachments;
        std::vector<Diagnostic> diagnostics;
        bool pendingChange = false;
        bool extensionsRequested = false;
        std::shared_ptr<const SemanticTokenSet> semantic;
        uint64_t semanticRequestedVersion = kNeverAsked;
        std::vector<InlayHint> inlayHints;
        uint64_t inlayRequestedVersion = kNeverAsked;
    };
    struct SettingsSnapshot {
        std::mutex mutex;
        Json values = Json::object();
    };
    struct ServerEntry {
        std::unique_ptr<lsp::LanguageServer> server;
        bool failedToStart = false;
        std::string failureMessage;
        std::set<std::string> openUris;
        bool fromExtensions = false;
    };

    void wireServer(lsp::LanguageServer* server, const std::string& name);
    std::string rootFolderOrHome() const;
    lsp::LanguageServer* serverForSpec(const lsp::ServerSpec& spec);
    lsp::LanguageServer* extensionServerForLanguage(const std::string& languageId);
    void serverEnded(const std::string& name, lsp::LanguageServer* instance);
    bool extensionsServeLanguage(const std::string& languageId);
    void attachExtensions(Document* document);
    void attachExtensionServer(const std::string& languageId, Document* document);
    void flushAllPending();
    void flushDocument(Document* document);
    void closeDocumentOnServers(const Tracked& tracked);
    void receivedDiagnostics(const std::vector<lsp::Diagnostic>& diagnostics, const std::string& uri, const std::string& server);
    void requestSemanticTokens(Document* document);
    void requestInlayHints(Document* document);
    std::vector<std::string> serversForMethod(const std::string& method, const Json& params, Tracked& tracked);
    static Attachment* attachmentNamed(Tracked& tracked, const std::string& name);

    std::vector<lsp::ServerSpec> specs_;
    std::map<std::string, ServerEntry> servers_;
    std::unordered_map<Document*, Tracked> tracked_;
    std::vector<Diagnostic> noDiagnostics_;
    std::vector<InlayHint> noInlayHints_;
    std::shared_ptr<SettingsSnapshot> settings_ = std::make_shared<SettingsSnapshot>();
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    uint64_t flushTimer_ = 0;
    uint64_t inlayTimer_ = 0;
};

}  // namespace gw::app
