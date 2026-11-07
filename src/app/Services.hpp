#pragma once

#include "BitmapMode.hpp"
#include "EditorSettings.hpp"
#include "Extensions.hpp"
#include "Font.hpp"
#include "Keybindings.hpp"
#include "Styling.hpp"
#include "Theme.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gw::app {

// What changed -> who has to look again (windows redraw, editors re-measure, menus refill).
enum class ServicesChange { Settings, Colors, Fonts, Extensions, Keybindings };

/// Everything the windows share: settings.json, the color scheme and its theme, fonts, installed
/// extensions and their grammars, keyboard shortcuts, and what is remembered between runs.
class Services {
public:
    static Services& shared();

    // ---- Start-up
    /// Reads the settings and extensions; installs the default extensions in the background the
    /// first time.
    void start();

    // ---- Settings
    /// What applies: settings.json with what bitmap mode locks (Vim on, no extra drawing).
    const EditorSettings& settings() const { return effective_; }
    /// Changes the saved settings and writes settings.json.
    void updateSettings(const std::function<void(EditorSettings&)>& change);
    /// settings.json as JSON, for language servers' `workspace/configuration` and extensions.
    const nlohmann::json& rawSettings() const { return raw_; }
    /// Looks at settings.json and keybindings.json and reloads them if they were edited.
    void checkFilesForChanges();

    // ---- Colors
    const bitmap::Scheme& scheme() const;
    const Theme& theme() const { return *theme_; }
    Stylist& stylist() { return *stylist_; }
    /// A workbench color of the scheme's theme ("editor.selectionBackground").
    Color color(const std::string& key, Color fallback) const { return theme_->color(key, fallback); }

    // ---- Fonts and sizes
    /// Device pixels per logical pixel of the display the windows are on (1 at 100%, 1.5 at 150%).
    void setDisplayScale(double scale);
    double displayScale() const { return displayScale_; }
    /// One pixel of the menus, frames and dialogs: 2 at 100% scaling.
    int chromePixel() const;
    ui::Font& chromeFont();
    /// The code's font: the pixel font at the editor size, or the font chosen in Options.
    std::shared_ptr<ui::Font> editorFont();
    std::shared_ptr<ui::Font> terminalFont();
    /// Whether the code is drawn in the built-in pixel font.
    bool usesPixelFont() const { return saved_.bitmapFont.empty() || !customFontAvailable_; }

    // ---- Extensions
    ExtensionManager& extensions() { return *extensions_; }
    GrammarRegistry& grammars() { return *grammars_; }
    bool installingDefaults() const { return installingDefaults_; }
    /// Downloads and installs from Open VSX; `done` runs on the main thread.
    void installExtension(const std::string& id, std::function<void(bool ok, std::string message)> done);
    void uninstallExtension(const std::string& id);
    void setExtensionEnabled(const std::string& id, bool enabled);
    bool isExtensionDisabled(const std::string& id) const;
    /// Re-reads the extensions folder (after installing or removing).
    void extensionsChanged();

    // ---- Keyboard shortcuts (keybindings.json and extensions' contributions)
    KeybindingTable& keybindings() { return keybindings_; }

    // ---- Files
    std::filesystem::path supportFolder() const;
    std::filesystem::path settingsPath() const;
    std::filesystem::path keybindingsPath() const;
    std::filesystem::path snippetsFolder() const;
    std::filesystem::path extensionLogsFolder() const;
    /// Writes keybindings.json with an explanation the first time it is opened.
    void ensureKeybindingsFile();

    // ---- Remembered between runs (state.json)
    std::vector<std::string> recentFolders() const;
    std::vector<std::string> recentFiles() const;
    void noteRecentFolder(const std::string& folder);
    void noteRecentFile(const std::string& file);
    /// Open files, carets and breakpoints of a folder.
    nlohmann::json folderState(const std::string& folder) const;
    void setFolderState(const std::string& folder, nlohmann::json state);
    nlohmann::json value(const std::string& key) const;
    void setValue(const std::string& key, nlohmann::json value);

    // ---- Listening
    int addListener(std::function<void(ServicesChange)> listener);
    void removeListener(int id);

private:
    Services();
    void loadSettings();
    void writeSettings();
    void refreshEffectiveSettings();
    void loadTheme();
    void rebuildStylist();
    void reloadKeybindings();
    void loadState();
    void saveState();
    void notify(ServicesChange change);
    void installDefaultExtensions();

    EditorSettings saved_;
    EditorSettings effective_;
    nlohmann::json raw_ = nlohmann::json::object();
    std::string lastSettingsText_;
    long long settingsStamp_ = 0;
    long long keybindingsStamp_ = 0;
    std::unique_ptr<Theme> theme_;
    std::unique_ptr<Stylist> stylist_;
    std::unique_ptr<ExtensionManager> extensions_;
    std::unique_ptr<GrammarRegistry> grammars_;
    std::vector<std::string> appliedDisabled_;
    bool installingDefaults_ = false;
    KeybindingTable keybindings_;
    nlohmann::json state_ = nlohmann::json::object();
    double displayScale_ = 1;
    bool customFontAvailable_ = true;
    std::map<int, std::function<void(ServicesChange)>> listeners_;
    int nextListener_ = 1;
};

}  // namespace gw::app
