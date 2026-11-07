#include "Services.hpp"
#include "Input.hpp"

#include "Jsonc.hpp"
#include "Loop.hpp"
#include "Paths.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace gw::app {

namespace fs = std::filesystem;

namespace {

/// Installed on first launch: VS Code's grammars for common languages, from Open VSX. (The
/// colors come from the scheme, so no themes are needed.)
const std::vector<std::string>& defaultExtensions() {
    static const std::vector<std::string> ids = {
        "vscode.cpp", "vscode.python", "vscode.javascript", "vscode.typescript", "vscode.json", "vscode.markdown",
        "vscode.shellscript", "vscode.make", "vscode.css", "vscode.html", "vscode.yaml", "vscode.rust", "vscode.go",
        "vscode.swift", "vscode.java", "vscode.objective-c", "vscode.diff", "vscode.git-base", "vscode.ini",
        "vscode.xml", "vscode.sql", "vscode.ruby", "vscode.php", "vscode.csharp", "vscode.lua", "vscode.docker",
        "vscode.scss", "vscode.less", "vscode.powershell", "vscode.dart", "vscode.log", "vscode.bat",
        "twxs.cmake",
    };
    return ids;
}

std::optional<std::string> readText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

bool writeText(const fs::path& path, const std::string& text) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    fs::path temporary = path;
    temporary += ".saving";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file << text;
        if (!file) {
            return false;
        }
    }
    fs::rename(temporary, path, error);
    return !error;
}

long long stampOf(const fs::path& path) {
    std::error_code error;
    auto time = fs::last_write_time(path, error);
    if (error) {
        return 0;
    }
    auto size = fs::file_size(path, error);
    return static_cast<long long>(time.time_since_epoch().count()) ^ (static_cast<long long>(size) << 40);
}

const char* kKeybindingsTemplate = R"json(// Your keyboard shortcuts, in VS Code's format. Save to apply.
//
//   { "key": "ctrl+shift+u", "command": "editor.action.transformToUppercase", "when": "editorTextFocus" }
//
// "key" can be a sequence ("ctrl+k ctrl+s"); "command" is a VS Code command id or an extension's;
// "args" are passed on; a command starting with "-" removes that command's binding.
// "when" understands && || ! == != =~ and editorTextFocus, terminalFocus, inputFocus,
// editorLangId, resourceExtname, resourceFilename, editorHasSelection, editorReadonly.
[
]
)json";

}  // namespace

Services& Services::shared() {
    static Services* services = new Services();
    return *services;
}

Services::Services() {
    std::error_code error;
    fs::create_directories(supportFolder(), error);
    extensions_ = std::make_unique<ExtensionManager>(supportFolder() / "extensions");
    extensions_->setUserSnippetsDirectory(snippetsFolder());
    grammars_ = std::make_unique<GrammarRegistry>(*extensions_);
    loadSettings();
    loadTheme();
    rebuildStylist();
    loadState();
}

void Services::start() {
    appliedDisabled_ = saved_.disabledExtensions;
    extensions_->setDisabled(std::set<std::string>(appliedDisabled_.begin(), appliedDisabled_.end()));
    extensions_->scan();
    reloadKeybindings();
    if (environmentValue("GLASSWORK_NO_DEFAULT_EXTENSIONS").empty()) {
        installDefaultExtensions();
    }
}

void Services::installDefaultExtensions() {
    std::vector<std::string> missing;
    for (const std::string& id : defaultExtensions()) {
        if (!extensions_->isInstalled(id)) {
            missing.push_back(id);
        }
    }
    if (missing.empty()) {
        return;
    }
    installingDefaults_ = true;
    fs::path folder = extensions_->directory();
    runInBackground([this, missing, folder] {
        for (const std::string& id : missing) {
            auto result = ExtensionManager::installFromOpenVSX(id, folder);
            if (!result.ok) {
                std::fprintf(stderr, "Glasswork Bitmap: %s\n", result.message.c_str());
            }
        }
        runOnMain([this] {
            installingDefaults_ = false;
            extensionsChanged();
        });
    });
}

// ---------------------------------------------------------------------------
// Settings

fs::path Services::supportFolder() const { return gw::supportFolder(); }
fs::path Services::settingsPath() const { return supportFolder() / "settings.json"; }
fs::path Services::keybindingsPath() const { return supportFolder() / "keybindings.json"; }
fs::path Services::snippetsFolder() const { return supportFolder() / "snippets"; }
fs::path Services::extensionLogsFolder() const { return supportFolder() / "extension-logs"; }

void Services::loadSettings() {
    std::optional<std::string> text = readText(settingsPath());
    if (!text) {
        refreshEffectiveSettings();
        writeSettings();
        return;
    }
    std::string error;
    saved_ = parseSettings(*text, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "Glasswork Bitmap: settings.json: %s\n", error.c_str());
    }
    nlohmann::json parsed = parseJsonc(*text);
    raw_ = parsed.is_object() ? parsed : nlohmann::json::object();
    lastSettingsText_ = *text;
    settingsStamp_ = stampOf(settingsPath());
    refreshEffectiveSettings();
}

void Services::refreshEffectiveSettings() {
    EditorSettings settings = saved_;
    settings.bitmapMode = true;
    effective_ = bitmap::withLocks(std::move(settings));
}

void Services::writeSettings() {
    std::string text = serializeSettings(saved_, raw_);
    lastSettingsText_ = text;
    writeText(settingsPath(), text);
    settingsStamp_ = stampOf(settingsPath());
    nlohmann::json parsed = parseJsonc(text);
    raw_ = parsed.is_object() ? parsed : nlohmann::json::object();
}

void Services::updateSettings(const std::function<void(EditorSettings&)>& change) {
    EditorSettings before = saved_;
    change(saved_);
    writeSettings();
    refreshEffectiveSettings();
    if (saved_.disabledExtensions != appliedDisabled_) {
        appliedDisabled_ = saved_.disabledExtensions;
        extensions_->setDisabled(std::set<std::string>(appliedDisabled_.begin(), appliedDisabled_.end()));
        extensionsChanged();
    }
    if (before.bitmapColors != saved_.bitmapColors) {
        loadTheme();
    }
    rebuildStylist();
    if (before.bitmapColors != saved_.bitmapColors) {
        notify(ServicesChange::Colors);
    }
    if (before.fontSize != saved_.fontSize || before.bitmapFont != saved_.bitmapFont ||
        before.terminalFontSize != saved_.terminalFontSize) {
        notify(ServicesChange::Fonts);
    }
    notify(ServicesChange::Settings);
}

void Services::checkFilesForChanges() {
    long long stamp = stampOf(settingsPath());
    if (stamp != 0 && stamp != settingsStamp_) {
        settingsStamp_ = stamp;
        std::optional<std::string> text = readText(settingsPath());
        if (text && *text != lastSettingsText_) {
            EditorSettings before = saved_;
            std::string error;
            saved_ = parseSettings(*text, &error);
            nlohmann::json parsed = parseJsonc(*text);
            raw_ = parsed.is_object() ? parsed : nlohmann::json::object();
            lastSettingsText_ = *text;
            refreshEffectiveSettings();
            if (saved_.disabledExtensions != appliedDisabled_) {
                appliedDisabled_ = saved_.disabledExtensions;
                extensions_->setDisabled(std::set<std::string>(appliedDisabled_.begin(), appliedDisabled_.end()));
                extensionsChanged();
            }
            if (before.bitmapColors != saved_.bitmapColors) {
                loadTheme();
            }
            rebuildStylist();
            notify(ServicesChange::Colors);
            notify(ServicesChange::Fonts);
            notify(ServicesChange::Settings);
        }
    }
    long long keys = stampOf(keybindingsPath());
    if (keys != keybindingsStamp_) {
        keybindingsStamp_ = keys;
        reloadKeybindings();
        notify(ServicesChange::Keybindings);
    }
}

// ---------------------------------------------------------------------------
// Colors

const bitmap::Scheme& Services::scheme() const {
    return bitmap::schemeNamed(saved_.bitmapColors);
}

void Services::loadTheme() {
    std::string error;
    theme_ = Theme::parse(scheme().themeJson, true, &error);
    if (!theme_) {
        theme_ = Theme::parse(bitmap::schemes().front().themeJson, true, &error);
    }
}

void Services::rebuildStylist() {
    stylist_ = std::make_unique<Stylist>(*theme_, effective_.colors);
}

// ---------------------------------------------------------------------------
// Fonts

void Services::setDisplayScale(double scale) {
    if (std::abs(scale - displayScale_) > 0.01 && scale > 0) {
        displayScale_ = scale;
        notify(ServicesChange::Fonts);
    }
}

int Services::chromePixel() const {
    // 2 at 100%, 3 at 150%, 4 at 200%: always whole device pixels.
    return std::max(2, static_cast<int>(std::floor(2 * displayScale_ + 0.25)));
}

ui::Font& Services::chromeFont() {
    return *ui::pixelFont(chromePixel());
}

std::shared_ptr<ui::Font> Services::editorFont() {
    if (!saved_.bitmapFont.empty()) {
        int height = std::max(6, static_cast<int>(std::lround(saved_.fontSize * displayScale_ * 96.0 / 72.0)));
        auto font = ui::trueTypeFont(saved_.bitmapFont, height);
        customFontAvailable_ = font != nullptr;
        if (font) {
            return font;
        }
    }
    customFontAvailable_ = !saved_.bitmapFont.empty() ? false : true;
    int pixel = std::max(1, static_cast<int>(std::lround(bitmap::pixelSize(saved_.fontSize) * displayScale_)));
    return ui::pixelFont(pixel);
}

std::shared_ptr<ui::Font> Services::terminalFont() {
    if (saved_.terminalFontSize <= 0) {
        return editorFont();
    }
    if (!saved_.bitmapFont.empty()) {
        int height = std::max(6, static_cast<int>(std::lround(saved_.terminalFontSize * displayScale_ * 96.0 / 72.0)));
        if (auto font = ui::trueTypeFont(saved_.bitmapFont, height)) {
            return font;
        }
    }
    int pixel = std::max(1, static_cast<int>(std::lround(bitmap::pixelSize(saved_.terminalFontSize) * displayScale_)));
    return ui::pixelFont(pixel);
}

// ---------------------------------------------------------------------------
// Extensions

void Services::extensionsChanged() {
    extensions_->scan();
    grammars_->clear();
    reloadKeybindings();
    notify(ServicesChange::Extensions);
}

void Services::installExtension(const std::string& id, std::function<void(bool, std::string)> done) {
    fs::path folder = extensions_->directory();
    std::string cleaned = id;
    cleaned.erase(0, cleaned.find_first_not_of(" \t\r\n"));
    cleaned.erase(cleaned.find_last_not_of(" \t\r\n") + 1);
    runInBackground([this, cleaned, folder, done] {
        auto result = ExtensionManager::installFromOpenVSX(cleaned, folder);
        runOnMain([this, result, done] {
            if (result.ok) {
                extensionsChanged();
            }
            done(result.ok, result.message);
        });
    });
}

void Services::uninstallExtension(const std::string& id) {
    extensions_->uninstall(id);
    extensionsChanged();
}

void Services::setExtensionEnabled(const std::string& id, bool enabled) {
    updateSettings([&](EditorSettings& settings) {
        auto& list = settings.disabledExtensions;
        list.erase(std::remove(list.begin(), list.end(), id), list.end());
        if (!enabled) {
            list.push_back(id);
        }
    });
}

bool Services::isExtensionDisabled(const std::string& id) const {
    const auto& list = saved_.disabledExtensions;
    return std::find(list.begin(), list.end(), id) != list.end();
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts

void Services::ensureKeybindingsFile() {
    std::error_code error;
    if (!fs::exists(keybindingsPath(), error)) {
        writeText(keybindingsPath(), kKeybindingsTemplate);
    }
}

void Services::reloadKeybindings() {
    keybindings_.clear();
    keybindings_.add(defaultKeybindings(), false);  // built-in shortcuts first; extensions and the user layer over them
    nlohmann::json contributed = nlohmann::json::parse(extensions_->contributedKeybindingsJson(), nullptr, false);
    if (contributed.is_array()) {
        keybindings_.add(contributed, false);
    }
    if (std::optional<std::string> text = readText(keybindingsPath())) {
        nlohmann::json user = parseJsonc(*text);
        if (user.is_array()) {
            keybindings_.add(user, true);
        }
    }
    keybindingsStamp_ = stampOf(keybindingsPath());
}

// ---------------------------------------------------------------------------
// State

void Services::loadState() {
    if (std::optional<std::string> text = readText(supportFolder() / "state.json")) {
        nlohmann::json parsed = nlohmann::json::parse(*text, nullptr, false);
        if (parsed.is_object()) {
            state_ = parsed;
        }
    }
}

void Services::saveState() {
    writeText(supportFolder() / "state.json", state_.dump(1));
}

std::vector<std::string> Services::recentFolders() const {
    std::vector<std::string> list;
    if (state_.contains("recentFolders") && state_["recentFolders"].is_array()) {
        for (const auto& item : state_["recentFolders"]) {
            if (item.is_string()) {
                list.push_back(item.get<std::string>());
            }
        }
    }
    return list;
}

std::vector<std::string> Services::recentFiles() const {
    std::vector<std::string> list;
    if (state_.contains("recentFiles") && state_["recentFiles"].is_array()) {
        for (const auto& item : state_["recentFiles"]) {
            if (item.is_string()) {
                list.push_back(item.get<std::string>());
            }
        }
    }
    return list;
}

namespace {
void pushRecent(nlohmann::json& state, const char* key, const std::string& value, size_t limit) {
    nlohmann::json list = nlohmann::json::array();
    list.push_back(value);
    if (state.contains(key) && state[key].is_array()) {
        for (const auto& item : state[key]) {
            if (item.is_string() && item.get<std::string>() != value && list.size() < limit) {
                list.push_back(item);
            }
        }
    }
    state[key] = list;
}
}  // namespace

void Services::noteRecentFolder(const std::string& folder) {
    pushRecent(state_, "recentFolders", folder, 15);
    saveState();
}

void Services::noteRecentFile(const std::string& file) {
    pushRecent(state_, "recentFiles", file, 15);
    saveState();
}

nlohmann::json Services::folderState(const std::string& folder) const {
    if (state_.contains("folders") && state_["folders"].is_object() && state_["folders"].contains(folder)) {
        return state_["folders"][folder];
    }
    return nlohmann::json::object();
}

void Services::setFolderState(const std::string& folder, nlohmann::json state) {
    if (!state_.contains("folders") || !state_["folders"].is_object()) {
        state_["folders"] = nlohmann::json::object();
    }
    state_["folders"][folder] = std::move(state);
    saveState();
}

nlohmann::json Services::value(const std::string& key) const {
    return state_.contains(key) ? state_[key] : nlohmann::json();
}

void Services::setValue(const std::string& key, nlohmann::json value) {
    state_[key] = std::move(value);
    saveState();
}

// ---------------------------------------------------------------------------
// Listening

int Services::addListener(std::function<void(ServicesChange)> listener) {
    int id = nextListener_++;
    listeners_[id] = std::move(listener);
    return id;
}

void Services::removeListener(int id) {
    listeners_.erase(id);
}

void Services::notify(ServicesChange change) {
    auto copy = listeners_;
    for (auto& [id, listener] : copy) {
        if (listeners_.count(id)) {
            listener(change);
        }
    }
}

}  // namespace gw::app
