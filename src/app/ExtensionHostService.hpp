#pragma once

#include "ExtensionHost.hpp"
#include "LanguageService.hpp"
#include "Overlays.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gw::app {

using Json = lsp::Json;

/// What the extension host service needs from its window.
class ExtensionHostDelegate {
public:
    virtual ~ExtensionHostDelegate() = default;
    // ---- Files
    virtual bool hostApplyWorkspaceEdit(const Json& edit) = 0;
    virtual void hostOpenFile(const std::string& path, long line, long character) = 0;
    virtual bool hostSaveFile(const std::string& path) = 0;
    virtual void hostSaveAll() = 0;
    // ---- Editor
    virtual bool hostRunBuiltinCommand(const std::string& identifier, const Json& arguments) = 0;
    virtual void hostSetSelections(const Json& selections, const std::string& path) = 0;
    virtual void hostSetDecorations(const std::string& path, const std::string& key, const Json& options, const Json& ranges) = 0;
    virtual void hostRevealRange(const Json& range, const std::string& path) = 0;
    virtual bool hostInsertSnippet(const std::string& snippet, const Json& ranges, const std::string& path) = 0;
    virtual void hostProvidersChanged() = 0;
    virtual void hostSetContextKey(const std::string& key, const std::string& value) = 0;
    // ---- Status and asking
    virtual void hostShowStatusMessage(const std::string& text, double seconds) = 0;
    virtual void hostStatusItemsChanged() = 0;
    virtual void hostShowDialog(DialogOverlay::Options options, std::function<void(int button, const std::string& text)> done) = 0;
    virtual void hostShowPalette(const std::string& placeholder, PaletteOverlay::Provider provider, std::function<void()> onDismiss) = 0;
};

/// One extension's status bar item.
struct ExtensionStatusItem {
    std::string identifier;
    std::string text;
    std::string tooltip;
    std::string command;
    int alignment = 1;
    double priority = 0;
};

/// Runs installed VS Code extensions in the Node.js extension host, one per window. The host starts
/// only when something needs it. Its providers reach the editor as a virtual language server;
/// commands, status items and messages show in the window, and output goes to log files.
class ExtensionHostService : public ExtensionLanguageProvider {
public:
    explicit ExtensionHostService(ExtensionHostDelegate& delegate);
    ~ExtensionHostService() override;

    // ---- Lifecycle
    void startIfExtensionsWantToRun();
    void shutdown();
    void extensionsDidChange();
    void restart();

    // ---- Commands
    std::vector<PaletteItem> commandItems(const std::string& query);
    void executeCommand(const std::string& identifier, const Json& arguments);
    std::string titleForCommand(const std::string& identifier);

    // ---- Editor and files
    void setEditorState(const Json& state);
    void filesDidChange(const std::vector<std::string>& paths);
    void settingsDidChange();
    Json allSettings();

    // ---- Logs
    void appendLog(const std::string& text, const std::string& channel);
    std::string logPathForChannel(const std::string& channel);
    std::vector<std::string> logChannels() const;

    // ---- Getters
    std::vector<ExtensionStatusItem> statusItems() const;
    bool hasRunnableExtensions();
    std::string summary() const;

    // ---- ExtensionLanguageProvider
    bool extensionsMayServeLanguage(const std::string& languageId) override;
    void prepareLanguage(const std::string& languageId, std::function<void(bool ok)> done) override;
    std::unique_ptr<lsp::ServerTransport> newTransportForLanguage(const std::string& languageId, const std::string& rootUri) override;

    std::string rootFolder;

private:
    struct IndexEntry {
        std::string identifier;
        std::set<std::string> activationEvents;
        std::set<std::string> languages;
        bool runsAtStartup = false;
    };
    struct ContributedCommand {
        std::string identifier;
        std::string title;
        bool hiddenFromPalette = false;
    };

    void buildIndexIfNeeded();
    std::string hostScriptPath() const;
    Json extensionDescriptions();
    Json configuration();
    Json languageByExtension();
    void ensureHost(std::function<void(bool ok)> done);
    void finishWaiting(bool ok);
    void hostExited(int code);
    void handleNotification(const std::string& method, const Json& params);
    void handleRequest(const std::string& method, const Json& params, ExtensionHost::Reply reply);
    void storeSetting(const Json& params);
    void showMessage(const Json& params, ExtensionHost::Reply reply);
    void showQuickPick(const Json& params, ExtensionHost::Reply reply);
    void showInputBox(const Json& params, ExtensionHost::Reply reply);

    ExtensionHostDelegate& delegate_;
    std::unique_ptr<ExtensionHost> host_;
    bool initializing_ = false;
    bool initialized_ = false;
    bool stopping_ = false;
    int restarts_ = 0;
    std::vector<std::function<void(bool)>> waiting_;
    std::set<std::string> providerLanguages_;
    bool providersForAnyLanguage_ = false;
    std::set<std::string> activatedLanguages_;
    std::vector<IndexEntry> index_;
    std::vector<ContributedCommand> contributedCommands_;
    std::map<std::string, std::string> registeredCommands_;
    std::map<std::string, ExtensionStatusItem> statusItems_;
    std::map<std::string, std::string> extensionStates_;
    std::set<std::string> openedChannels_;
    std::set<std::string> knownChannels_;
    bool indexBuilt_ = false;
    std::string lastEditorState_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace gw::app
