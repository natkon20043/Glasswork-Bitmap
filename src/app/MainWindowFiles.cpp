#include "MainWindow.hpp"

#include "App.hpp"
#include "BitmapMode.hpp"
#include "Fuzzy.hpp"
#include "Git.hpp"
#include "Image.hpp"
#include "Loop.hpp"
#include "Paths.hpp"
#include "Platform.hpp"
#include "Services.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace gw::app {

namespace fs = std::filesystem;

namespace {

/// A path as a key: one spelling per file (case and separators don't matter on Windows).
std::string pathKey(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    std::error_code error;
    fs::path absolute = fs::absolute(pathFromUtf8(path), error);
    std::string key = utf8FromPath(absolute.lexically_normal());
#ifdef _WIN32
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

/// Names in the order a person expects: case ignored, numbers by value ("file2" before "file10").
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[j]);
        if (std::isdigit(x) && std::isdigit(y)) {
            size_t endA = i, endB = j;
            while (endA < a.size() && std::isdigit(static_cast<unsigned char>(a[endA]))) {
                ++endA;
            }
            while (endB < b.size() && std::isdigit(static_cast<unsigned char>(b[endB]))) {
                ++endB;
            }
            std::string numberA = a.substr(i, endA - i), numberB = b.substr(j, endB - j);
            numberA.erase(0, std::min(numberA.find_first_not_of('0'), numberA.size() - 1));
            numberB.erase(0, std::min(numberB.find_first_not_of('0'), numberB.size() - 1));
            if (numberA.size() != numberB.size()) {
                return numberA.size() < numberB.size();
            }
            if (numberA != numberB) {
                return numberA < numberB;
            }
            i = endA;
            j = endB;
            continue;
        }
        int lowerX = std::tolower(x), lowerY = std::tolower(y);
        if (lowerX != lowerY) {
            return lowerX < lowerY;
        }
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

bool isNoisyFolder(const std::string& name) {
    static const std::set<std::string> noisy = {".git", "node_modules", ".cache", "__pycache__", ".venv", "venv", "target", ".gradle",
                                                 "build", "dist", "out", ".next", ".idea", ".vs", "bin", "obj", "DerivedData"};
    return noisy.count(name) > 0;
}

bool looksBinary(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    char buffer[8192];
    file.read(buffer, sizeof(buffer));
    std::streamsize count = file.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
        if (buffer[index] == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tabs

Tab* MainWindow::currentTab() const {
    if (selected_ < 0 || static_cast<size_t>(selected_) >= tabs_.size()) {
        return nullptr;
    }
    return tabs_[static_cast<size_t>(selected_)].get();
}

Tab* MainWindow::tabForPath(const std::string& path) const {
    if (path.empty()) {
        return nullptr;
    }
    std::string key = pathKey(path);
    for (const auto& tab : tabs_) {
        if (tab->kind != Tab::Kind::Preview && !tab->path().empty() && pathKey(tab->path()) == key) {
            return tab.get();
        }
    }
    return nullptr;
}

void MainWindow::attachDocument(Tab& tab) {
    Services& services = Services::shared();
    tab.highlighter = std::make_unique<Highlighter>(services.grammars().forLanguage(tab.document->languageId));
    tab.configuration = services.extensions().configuration(tab.document->languageId);
    Document* document = tab.document.get();
    uint64_t identifier = tab.identifier;
    document->onChange = [this, document, identifier](const Change& change) {
        for (auto& candidate : tabs_) {
            if (candidate->identifier == identifier) {
                candidate->highlighter->didChange(change);
                break;
            }
        }
        editor_.documentChanged(document, change);
        language_->documentDidChange(document);
        dirty_ = true;
    };
}

Tab& MainWindow::addTab(std::unique_ptr<Tab> tab, bool select) {
    tab->identifier = nextTabId_++;
    if (tab->kind == Tab::Kind::Text) {
        attachDocument(*tab);
        if (!tab->path().empty()) {
            tab->editorConfig = editorConfigFor(pathFromUtf8(tab->path()));
        }
    }
    size_t index = selected_ >= 0 ? static_cast<size_t>(selected_) + 1 : tabs_.size();
    index = std::min(index, tabs_.size());
    tabs_.insert(tabs_.begin() + static_cast<long>(index), std::move(tab));
    Tab& added = *tabs_[index];
    if (added.kind == Tab::Kind::Text) {
        if (!added.path().empty()) {
            language_->trackDocument(added.document.get());
            auto saved = breakpointStore_.find(added.path());
            if (saved != breakpointStore_.end()) {
                added.document->breakpointLines = saved->second;
            }
        }
        refreshGit(added);
    }
    if (select || selected_ < 0) {
        showTab(static_cast<long>(index));
    } else if (static_cast<long>(index) <= selected_) {
        ++selected_;
    }
    scheduleEditorStateForExtensions();
    return added;
}

void MainWindow::showTab(long index) {
    if (Tab* previous = currentTab(); previous && previous->kind == Tab::Kind::Text && editor_.document() == previous->document.get()) {
        previous->scrollX = editor_.scrollX();
        previous->scrollY = editor_.scrollY();
    }
    suggestions_.hide();
    hover_.hide();
    changeBox_.hide();
    signatureLine_ = -1;
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) {
        selected_ = tabs_.empty() ? -1 : std::clamp(index, 0L, static_cast<long>(tabs_.size()) - 1);
        if (selected_ < 0) {
            editor_.setDocument(nullptr, nullptr, LanguageConfiguration());
            findBar_->close();
            refresh();
            return;
        }
        index = selected_;
    }
    selected_ = index;
    Tab& tab = *tabs_[static_cast<size_t>(index)];
    if (tab.kind == Tab::Kind::Text) {
        if (editor_.document() != tab.document.get()) {
            editor_.setDocument(tab.document.get(), tab.highlighter.get(), tab.configuration);
            applyEditorConfig(tab);
            layout();
            editor_.scrollTo(tab.scrollX, tab.scrollY);
        }
        editor_.setDiagnostics(language_->diagnosticsFor(tab.document.get()));
        editor_.setInlayHints(language_->inlayHintsFor(tab.document.get()));
        editor_.setSemanticTokens(language_->semanticTokensFor(tab.document.get()));
        editor_.setLineChanges(gutterChanges(tab.hunks));
        editor_.setDecorationGroups(tab.decorations);
        updateExecutionMarker();
        if (findBar_->visible()) {
            findBar_->documentChanged();
        }
    } else {
        editor_.setDocument(nullptr, nullptr, LanguageConfiguration());
        findBar_->close();
    }
    if (focus_ == Focus::Find && !findBar_->visible()) {
        setFocus(Focus::Editor);
    }
    refresh();
    updateStatus();
    scheduleEditorStateForExtensions();
}

void MainWindow::refresh() {
    updateTitle();
    Tab* tab = currentTab();
    std::string file = tab ? tab->path() : std::string();
    if (panelOpen_ && panel_ == SidePanel::Files && file != listedFile_) {
        reloadFiles();
    } else if (panelOpen_ && panel_ == SidePanel::Issues) {
        reloadProblems();
    }
    dirty_ = true;
}

void MainWindow::closeTab(long index) {
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) {
        return;
    }
    Tab& tab = *tabs_[static_cast<size_t>(index)];
    if (!tab.dirty()) {
        removeTab(index);
        return;
    }
    DialogOverlay::Options options;
    options.title = "Save Changes";
    options.lines = {"Save the changes to " + tab.title() + "?", "Your changes are lost if you don't save them."};
    options.buttons = {"&Save", "&Don't Save", "Cancel"};
    uint64_t identifier = tab.identifier;
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, identifier](int button, const std::string&) {
        auto find = [this, identifier]() -> long {
            for (size_t position = 0; position < tabs_.size(); ++position) {
                if (tabs_[position]->identifier == identifier) {
                    return static_cast<long>(position);
                }
            }
            return -1;
        };
        long position = find();
        if (position < 0) {
            return;
        }
        if (button == 0) {
            saveTab(*tabs_[static_cast<size_t>(position)], [this, find](bool ok) {
                long now = find();
                if (ok && now >= 0) {
                    removeTab(now);
                }
            });
        } else if (button == 1) {
            removeTab(position);
        }
    }));
}

void MainWindow::removeTab(long index) {
    if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) {
        return;
    }
    Tab& tab = *tabs_[static_cast<size_t>(index)];
    if (tab.kind == Tab::Kind::Text && !tab.path().empty()) {
        closedPaths_.push_back(tab.path());
        if (closedPaths_.size() > 30) {
            closedPaths_.erase(closedPaths_.begin());
        }
        breakpointStore_[tab.path()] = tab.document->breakpointLines;
        if (breakpointStore_[tab.path()].empty()) {
            breakpointStore_.erase(tab.path());
        }
    }
    if (tab.kind == Tab::Kind::Text) {
        if (tab.diffTimer) {
            cancelTimer(tab.diffTimer);
        }
        if (tab.autoSaveTimer) {
            cancelTimer(tab.autoSaveTimer);
        }
        language_->untrackDocument(tab.document.get());
        tab.document->onChange = nullptr;
        if (editor_.document() == tab.document.get()) {
            editor_.setDocument(nullptr, nullptr, LanguageConfiguration());
        }
    }
    bool wasSelected = index == selected_;
    tabs_.erase(tabs_.begin() + index);
    if (wasSelected) {
        selected_ = -1;
        showTab(tabs_.empty() ? -1 : std::min(index, static_cast<long>(tabs_.size()) - 1));
    } else if (index < selected_) {
        --selected_;
        refresh();
    } else {
        refresh();
    }
    saveSession();
    scheduleEditorStateForExtensions();
}

void MainWindow::closeOtherTabs() {
    Tab* keep = currentTab();
    if (!keep) {
        return;
    }
    uint64_t identifier = keep->identifier;
    for (long index = static_cast<long>(tabs_.size()) - 1; index >= 0; --index) {
        if (tabs_[static_cast<size_t>(index)]->identifier != identifier && !tabs_[static_cast<size_t>(index)]->dirty()) {
            removeTab(index);
        }
    }
}

void MainWindow::reopenClosedTab() {
    while (!closedPaths_.empty()) {
        std::string path = closedPaths_.back();
        closedPaths_.pop_back();
        std::error_code error;
        if (fs::exists(pathFromUtf8(path), error) && !tabForPath(path)) {
            openPath(path);
            return;
        }
    }
    beep();
}

void MainWindow::newFile() {
    auto tab = std::make_unique<Tab>();
    tab->document = std::make_unique<Document>();
    int number = 1;
    for (auto& other : tabs_) {
        if (other->kind == Tab::Kind::Text && other->path().empty()) {
            ++number;
        }
    }
    tab->document->title = "Untitled-" + std::to_string(number);
    tab->document->languageId = "plaintext";
    addTab(std::move(tab), true);
    setFocus(Focus::Editor);
}

void MainWindow::openFileDialog() {
    std::string start = folder_;
    if (Tab* tab = currentTab(); tab && !tab->path().empty()) {
        start = utf8FromPath(pathFromUtf8(tab->path()).parent_path());
    }
    std::weak_ptr<bool> alive = alive_;
    chooseFiles(window_, start, [this, alive](std::vector<std::string> paths) {
        if (alive.expired()) {
            return;
        }
        for (const std::string& path : paths) {
            openPath(path);
        }
    });
}

void MainWindow::openFolderDialog(bool newWindow) {
    std::weak_ptr<bool> alive = alive_;
    chooseFolder(window_, folder_.empty() ? homeFolder() : folder_, [this, alive, newWindow](std::optional<std::string> folder) {
        if (alive.expired() || !folder) {
            return;
        }
        if (newWindow) {
            app_.openWindow(*folder, {});
        } else {
            openFolder(*folder);
        }
    });
}

void MainWindow::openPath(const std::string& given, long line, long column) {
    if (given.empty()) {
        return;
    }
    fs::path path = pathFromUtf8(given);
    if (path.is_relative() && !folder_.empty()) {
        path = pathFromUtf8(folder_) / path;
    }
    std::error_code error;
    path = fs::absolute(path, error).lexically_normal();
    std::string text = utf8FromPath(path);
    if (fs::is_directory(path, error)) {
        if (folder_.empty()) {
            openFolder(text);
        } else if (pathKey(text) != pathKey(folder_)) {
            app_.openWindow(text, {});
        }
        return;
    }
    auto jump = [this, line, column] {
        if (line > 0 && currentTab() && currentTab()->kind == Tab::Kind::Text) {
            const TextBuffer& buffer = currentTab()->document->buffer();
            size_t target = std::min(static_cast<size_t>(line - 1), buffer.lineCount() - 1);
            std::string_view content = buffer.line(target);
            // Columns from tools count characters; walk to the byte.
            size_t byte = 0;
            for (long counted = 1; counted < column && byte < content.size(); ++counted) {
                ++byte;
                while (byte < content.size() && (static_cast<unsigned char>(content[byte]) & 0xC0) == 0x80) {
                    ++byte;
                }
            }
            editor_.jumpToPosition({target, byte});
        }
    };
    if (Tab* existing = tabForPath(text)) {
        for (size_t index = 0; index < tabs_.size(); ++index) {
            if (tabs_[index].get() == existing) {
                showTab(static_cast<long>(index));
            }
        }
        jump();
        setFocus(Focus::Editor);
        return;
    }
    if (!fs::exists(path, error)) {
        showError("Can't open " + utf8FromPath(path.filename()), "The file doesn't exist: " + abbreviateHome(text));
        return;
    }
    if (ui::isImageFile(path)) {
        openImage(text);
        return;
    }
    if (looksBinary(path)) {
        showError("Can't open " + utf8FromPath(path.filename()), "It is a binary file, not text.");
        return;
    }
    std::string problem;
    std::optional<Document> document = Document::open(path, &problem);
    if (!document) {
        showError("Can't open " + utf8FromPath(path.filename()), problem);
        return;
    }
    auto tab = std::make_unique<Tab>();
    tab->document = std::make_unique<Document>(std::move(*document));
    if (auto language = Services::shared().extensions().languageForFile(path)) {
        tab->document->languageId = *language;
    } else if (tab->document->languageId.empty()) {
        tab->document->languageId = "plaintext";
    }
    // Settings and keybindings are JSON with comments.
    if (pathKey(text) == pathKey(utf8FromPath(Services::shared().settingsPath())) ||
        pathKey(text) == pathKey(utf8FromPath(Services::shared().keybindingsPath()))) {
        tab->document->languageId = "jsonc";
    }
    addTab(std::move(tab), true);
    Services::shared().noteRecentFile(text);
    jump();
    setFocus(Focus::Editor);
}

void MainWindow::openImage(const std::string& path) {
    auto tab = std::make_unique<Tab>();
    tab->kind = Tab::Kind::Image;
    std::string error;
    if (!ui::loadImage(pathFromUtf8(path), tab->pixels, tab->imageWidth, tab->imageHeight, &error)) {
        showError("Can't open " + utf8FromPath(pathFromUtf8(path).filename()), error);
        return;
    }
    // A picture tab keeps its path in a read-only document so it is found, listed and restored like a file.
    tab->document = std::make_unique<Document>(Document::readOnlyText("", utf8FromPath(pathFromUtf8(path).filename())));
    tab->previewOf = path;
    addTab(std::move(tab), true);
}

// The picture tab's path is in previewOf; Tab::path only knows documents with paths.

bool MainWindow::saveTab(Tab& tab, std::function<void(bool)> done) {
    if (tab.kind != Tab::Kind::Text) {
        if (done) {
            done(true);
        }
        return true;
    }
    if (tab.path().empty()) {
        saveTabAs(tab, done);
        return false;
    }
    prepareForSaving(tab);
    Document* document = tab.document.get();
    bool format = Services::shared().settings().formatOnSave && language_->serverSupports("documentFormattingProvider", document) &&
                  currentTab() == &tab;
    if (!format) {
        writeTab(tab, done);
        return true;
    }
    // A slow or silent server mustn't hold the save up: save anyway after a moment.
    uint64_t identifier = tab.identifier;
    auto finished = std::make_shared<bool>(false);
    auto finish = [this, identifier, finished, done] {
        if (*finished) {
            return;
        }
        *finished = true;
        for (auto& candidate : tabs_) {
            if (candidate->identifier == identifier) {
                writeTab(*candidate, done);
                return;
            }
        }
    };
    std::weak_ptr<bool> alive = alive_;
    afterDelay(1.5, [alive, finish] {
        if (!alive.expired()) {
            finish();
        }
    });
    formatDocument(document, finish);
    return true;
}

void MainWindow::writeTab(Tab& tab, std::function<void(bool)> done) {
    Document* document = tab.document.get();
    uint64_t identifier = tab.identifier;
    auto write = [this, identifier, done] {
        for (auto& candidate : tabs_) {
            if (candidate->identifier != identifier) {
                continue;
            }
            std::string error;
            if (candidate->document->save(&error)) {
                afterSave(*candidate);
                if (done) {
                    done(true);
                }
            } else {
                showError("Couldn't save " + candidate->title(), error);
                if (done) {
                    done(false);
                }
            }
            return;
        }
    };
    if (document->fileChangedOnDisk()) {
        DialogOverlay::Options options;
        options.title = "File Changed";
        options.lines = {tab.title() + " was changed by another program since it was opened.", "Save your version over it?"};
        options.buttons = {"&Overwrite", "Cancel"};
        pushOverlay(std::make_unique<DialogOverlay>(options, [write, done](int button, const std::string&) {
            if (button == 0) {
                write();
            } else if (done) {
                done(false);
            }
        }));
        return;
    }
    write();
}

void MainWindow::afterSave(Tab& tab) {
    language_->documentDidSave(tab.document.get());
    std::string path = tab.path();
    if (pathKey(path) == pathKey(utf8FromPath(Services::shared().settingsPath())) ||
        pathKey(path) == pathKey(utf8FromPath(Services::shared().keybindingsPath()))) {
        Services::shared().checkFilesForChanges();
    }
    tab.askedAboutDiskChange = false;
    refreshGit(tab);
    setStatusMessage("Saved " + tab.title(), 2);
    scheduleEditorStateForExtensions();
    refresh();
}

void MainWindow::saveTabAs(Tab& tab, std::function<void(bool)> done) {
    std::string suggested = tab.path();
    if (suggested.empty()) {
        suggested = utf8FromPath((folder_.empty() ? pathFromUtf8(homeFolder()) : pathFromUtf8(folder_)) / pathFromUtf8(tab.title()));
    }
    uint64_t identifier = tab.identifier;
    std::weak_ptr<bool> alive = alive_;
    chooseSaveLocation(window_, suggested, [this, alive, identifier, done](std::optional<std::string> chosen) {
        if (alive.expired()) {
            return;
        }
        Tab* tab = nullptr;
        for (auto& candidate : tabs_) {
            if (candidate->identifier == identifier) {
                tab = candidate.get();
            }
        }
        if (!chosen || !tab) {
            if (done) {
                done(false);
            }
            return;
        }
        bool wasUntitled = tab->path().empty();
        prepareForSaving(*tab);
        std::string error;
        if (!tab->document->saveAs(pathFromUtf8(*chosen), &error)) {
            showError("Couldn't save " + tab->title(), error);
            if (done) {
                done(false);
            }
            return;
        }
        if (auto language = Services::shared().extensions().languageForFile(pathFromUtf8(*chosen))) {
            tab->document->languageId = *language;
        }
        tab->highlighter->setGrammar(Services::shared().grammars().forLanguage(tab->document->languageId));
        tab->configuration = Services::shared().extensions().configuration(tab->document->languageId);
        if (wasUntitled) {
            language_->trackDocument(tab->document.get());
        } else {
            language_->documentDidMove(tab->document.get());
        }
        if (tab == currentTab()) {
            editor_.setDocument(nullptr, nullptr, LanguageConfiguration());
            showTab(selected_);
        }
        Services::shared().noteRecentFile(*chosen);
        afterSave(*tab);
        reloadFiles();
        if (done) {
            done(true);
        }
    });
}

void MainWindow::saveAll() {
    Tab* untitled = nullptr;
    for (auto& tab : tabs_) {
        if (!tab->dirty()) {
            continue;
        }
        if (tab->path().empty()) {
            untitled = untitled ? untitled : tab.get();
            continue;
        }
        saveTab(*tab);
    }
    if (untitled) {
        for (size_t index = 0; index < tabs_.size(); ++index) {
            if (tabs_[index].get() == untitled) {
                showTab(static_cast<long>(index));
            }
        }
        saveTabAs(*untitled);
    }
}

void MainWindow::saveAllQuietly() {
    for (auto& tab : tabs_) {
        if (tab->dirty() && !tab->path().empty() && !tab->document->fileChangedOnDisk()) {
            prepareForSaving(*tab);
            std::string error;
            if (tab->document->save(&error)) {
                afterSave(*tab);
            }
        }
    }
}

void MainWindow::prepareForSaving(Tab& tab) {
    const EditorSettings& settings = Services::shared().settings();
    bool trim = tab.editorConfig.trimTrailingWhitespace.value_or(settings.trimTrailingWhitespace);
    bool finalNewline = tab.editorConfig.insertFinalNewline.value_or(settings.insertFinalNewline);
    if (!trim && !finalNewline) {
        return;
    }
    Document& document = *tab.document;
    const TextBuffer& buffer = document.buffer();
    std::vector<std::pair<Range, std::string>> edits;
    if (finalNewline && buffer.lineCount() > 0 && !buffer.line(buffer.lineCount() - 1).empty()) {
        size_t last = buffer.lineCount() - 1;
        edits.push_back({{{last, buffer.line(last).size()}, {last, buffer.line(last).size()}}, "\n"});
    }
    if (trim) {
        for (size_t line = buffer.lineCount(); line-- > 0;) {
            std::string_view text = buffer.line(line);
            size_t end = text.size();
            while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
                --end;
            }
            if (end < text.size() && !(document.selection.caret.line == line && document.selection.caret.column > end && &tab == currentTab())) {
                edits.push_back({{{line, end}, {line, text.size()}}, ""});
            }
        }
    }
    if (edits.empty()) {
        return;
    }
    Selection selection = document.selection;
    auto apply = [&edits](Document& target) {
        for (const auto& [range, text] : edits) {
            target.replace(range, text);
        }
    };
    if (&tab == currentTab()) {
        editor_.editDocument(apply);
    } else {
        document.beginUndoGroup();
        apply(document);
        document.endUndoGroup();
    }
    document.selection = {document.buffer().clamp(selection.anchor), document.buffer().clamp(selection.caret)};
}

void MainWindow::documentEdited(Tab& tab) {
    if (&tab == currentTab() && findBar_->visible()) {
        findBar_->documentChanged();
    }
    scheduleDiff(tab);
    scheduleAutoSave(tab);
    scheduleEditorStateForExtensions();
    dirty_ = true;
}

void MainWindow::scheduleAutoSave(Tab& tab) {
    if (!Services::shared().settings().autoSave || tab.path().empty()) {
        return;
    }
    if (tab.autoSaveTimer) {
        cancelTimer(tab.autoSaveTimer);
    }
    uint64_t identifier = tab.identifier;
    std::weak_ptr<bool> alive = alive_;
    tab.autoSaveTimer = afterDelay(1.0, [this, alive, identifier] {
        if (alive.expired()) {
            return;
        }
        for (auto& candidate : tabs_) {
            if (candidate->identifier == identifier) {
                candidate->autoSaveTimer = 0;
                if (candidate->dirty() && !candidate->document->fileChangedOnDisk()) {
                    std::string error;
                    prepareForSaving(*candidate);
                    if (candidate->document->save(&error)) {
                        afterSave(*candidate);
                    }
                }
            }
        }
    });
}

void MainWindow::openSettingsFile() {
    fs::path path = Services::shared().settingsPath();
    std::error_code error;
    if (!fs::exists(path, error)) {
        fs::create_directories(path.parent_path(), error);
        std::ofstream(path, std::ios::binary) << "{\n    \"editor.fontSize\": 13\n}\n";
    }
    openPath(utf8FromPath(path));
}

void MainWindow::openKeybindingsFile() {
    Services::shared().ensureKeybindingsFile();
    openPath(utf8FromPath(Services::shared().keybindingsPath()));
}

void MainWindow::showPreview() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return;
    }
    if (tab->path().empty()) {
        showMessage("Markdown Preview", "Save the file first.");
        return;
    }
    std::string path = tab->path();
    for (size_t index = 0; index < tabs_.size(); ++index) {
        if (tabs_[index]->kind == Tab::Kind::Preview && pathKey(tabs_[index]->previewOf) == pathKey(path)) {
            showTab(static_cast<long>(index));
            return;
        }
    }
    auto preview = std::make_unique<Tab>();
    preview->kind = Tab::Kind::Preview;
    preview->previewOf = path;
    addTab(std::move(preview), true);
}

// ---------------------------------------------------------------------------
// Changes on disk

void MainWindow::filesChangedOnDisk(const std::vector<std::string>& paths) {
    bool gitChanged = false;
    bool listChanged = false;
    for (const std::string& path : paths) {
        fs::path changed = pathFromUtf8(path);
        if (changed.filename() == ".git") {
            gitChanged = true;
        }
        if (pathKey(utf8FromPath(changed.parent_path())) == pathKey(listedFolder_)) {
            listChanged = true;
        }
    }
    for (auto& tab : tabs_) {
        checkDiskChange(*tab);
        if (gitChanged) {
            refreshGit(*tab);
        }
    }
    language_->filesDidChange(paths);
    extensionHost_->filesDidChange(paths);
    if (listChanged && panel_ == SidePanel::Files) {
        reloadFiles();
    }
    // New and removed files change what Quick Open finds.
    bool structural = false;
    for (const std::string& path : paths) {
        std::error_code error;
        if (!fs::exists(pathFromUtf8(path), error) || fs::is_directory(pathFromUtf8(path), error)) {
            structural = true;
        } else if (fileIndex_) {
            std::string relative = relativePath(path);
            structural = structural || std::find(fileIndex_->begin(), fileIndex_->end(), relative) == fileIndex_->end();
        }
    }
    if (structural) {
        rebuildFileIndex();
    }
    dirty_ = true;
}

void MainWindow::checkDiskChange(Tab& tab) {
    if (tab.kind != Tab::Kind::Text || tab.path().empty() || !tab.document->fileChangedOnDisk()) {
        return;
    }
    std::error_code error;
    if (!fs::exists(pathFromUtf8(tab.path()), error)) {
        return;  // deleted or being replaced: keep the text, a save writes it back
    }
    if (!tab.document->isDirty()) {
        std::string problem;
        if (tab.document->reloadFromDisk(&problem) == Document::ReloadResult::Reloaded) {
            if (&tab == currentTab()) {
                editor_.selectionDidChange();
            }
            refreshGit(tab);
        }
        return;
    }
    if (tab.askedAboutDiskChange) {
        return;
    }
    tab.askedAboutDiskChange = true;
    uint64_t identifier = tab.identifier;
    DialogOverlay::Options options;
    options.title = "File Changed";
    options.lines = {tab.title() + " was changed by another program, and you have unsaved changes.", "Load the version on disk?"};
    options.buttons = {"&Load It", "&Keep Mine"};
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, identifier](int button, const std::string&) {
        for (auto& candidate : tabs_) {
            if (candidate->identifier != identifier) {
                continue;
            }
            if (button == 0) {
                std::string problem;
                candidate->document->reloadFromDisk(&problem);
                refreshGit(*candidate);
            } else {
                candidate->document->acknowledgeDiskChange();
            }
            candidate->askedAboutDiskChange = false;
        }
        dirty_ = true;
    }));
}

// ---------------------------------------------------------------------------
// Folders and sessions

void MainWindow::closeTabsForFolderSwitch(std::function<void(bool)> done) {
    size_t unsaved = 0;
    for (auto& tab : tabs_) {
        unsaved += tab->dirty() ? 1 : 0;
    }
    if (unsaved == 0) {
        done(true);
        return;
    }
    DialogOverlay::Options options;
    options.title = "Save Changes";
    options.lines = {std::to_string(unsaved) + (unsaved == 1 ? " file has" : " files have") + " unsaved changes.",
                     "Save them before closing?"};
    options.buttons = {"&Save All", "&Don't Save", "Cancel"};
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, done](int button, const std::string&) {
        if (button == 0) {
            bool untitled = false;
            for (auto& tab : tabs_) {
                if (tab->dirty() && tab->path().empty()) {
                    untitled = true;
                }
            }
            if (untitled) {
                showMessage("Save Changes", "Some files have never been saved. Save them with Save As first.");
                done(false);
                return;
            }
            saveAllQuietly();
            for (auto& tab : tabs_) {
                if (tab->dirty()) {
                    done(false);
                    return;
                }
            }
            done(true);
        } else {
            done(button == 1);
        }
    }));
}

void MainWindow::openFolder(const std::string& given) {
    std::error_code error;
    fs::path path = fs::absolute(pathFromUtf8(given), error).lexically_normal();
    std::string folder = utf8FromPath(path);
    if (folder.size() > 1 && (folder.back() == '\\' || folder.back() == '/') && !(folder.size() == 3 && folder[1] == ':')) {
        folder.pop_back();
    }
    if (!fs::is_directory(path, error)) {
        showError("Can't open the folder", abbreviateHome(folder) + " isn't a folder.");
        return;
    }
    if (pathKey(folder) == pathKey(folder_)) {
        return;
    }
    if (folder_.empty() && tabs_.empty()) {
        switchFolder(folder);
        return;
    }
    // Another window may already have it.
    for (const auto& window : app_.windows()) {
        if (window.get() != this && pathKey(window->folder()) == pathKey(folder)) {
            SDL_RaiseWindow(window->sdlWindow());
            return;
        }
    }
    closeTabsForFolderSwitch([this, folder](bool ok) {
        if (ok) {
            switchFolder(folder);
        }
    });
}

void MainWindow::requestClose(std::function<void(bool)> done) {
    closeTabsForFolderSwitch(std::move(done));
}

void MainWindow::switchFolder(const std::string& folder) {
    saveSession();
    while (!tabs_.empty()) {
        Tab& tab = *tabs_.back();
        if (tab.kind == Tab::Kind::Text) {
            language_->untrackDocument(tab.document.get());
            tab.document->onChange = nullptr;
        }
        tabs_.pop_back();
    }
    selected_ = -1;
    editor_.setDocument(nullptr, nullptr, LanguageConfiguration());
    findBar_->close();
    folder_ = folder;
    listedFolder_.clear();
    language_->rootFolder = folder;
    extensionHost_->rootFolder = folder;
    terminal_.projectFolder = folder;
    navigation_ = NavigationHistory();
    gitRoot_.clear();
    std::weak_ptr<bool> alive = alive_;
    watcher_ = std::make_unique<FolderWatcher>(folder, [this, alive](std::vector<std::string> paths) {
        if (!alive.expired()) {
            filesChangedOnDisk(paths);
        }
    });
    runInBackground([this, alive, folder] {
        git::Status status = git::readStatus(folder);
        std::string root = status.isRepository ? status.root : std::string();
        runOnMain([this, alive, folder, root] {
            if (alive.expired() || folder != folder_) {
                return;
            }
            gitRoot_ = root;
            for (auto& tab : tabs_) {
                refreshGit(*tab);
            }
        });
    });
    rebuildFileIndex();
    Services::shared().noteRecentFolder(folder);
    loadBreakpoints();
    restoreSession();
    updateTitle();
    reloadSidePanel();
    extensionHost_->startIfExtensionsWantToRun();
    dirty_ = true;
}

void MainWindow::saveSession() {
    if (folder_.empty()) {
        return;
    }
    nlohmann::json state = nlohmann::json::object();
    nlohmann::json tabs = nlohmann::json::array();
    long selectedIndex = -1;
    for (size_t index = 0; index < tabs_.size(); ++index) {
        Tab& tab = *tabs_[index];
        std::string path = tab.kind == Tab::Kind::Image ? tab.previewOf : tab.path();
        if (path.empty() || tab.kind == Tab::Kind::Preview) {
            continue;
        }
        if (static_cast<long>(index) == selected_) {
            selectedIndex = static_cast<long>(tabs.size());
        }
        nlohmann::json entry = {{"path", path}};
        if (tab.kind == Tab::Kind::Text) {
            double scrollY = &tab == currentTab() ? editor_.scrollY() : tab.scrollY;
            entry["line"] = tab.document->selection.caret.line;
            entry["column"] = tab.document->selection.caret.column;
            entry["scrollY"] = scrollY;
        }
        tabs.push_back(entry);
    }
    state["tabs"] = tabs;
    state["selected"] = selectedIndex;
    state["terminal"] = terminalVisible_;
    state["breakpoints"] = breakpointsJson();
    Services::shared().setFolderState(folder_, state);
}

void MainWindow::restoreSession() {
    nlohmann::json state = Services::shared().folderState(folder_);
    if (!state.is_object()) {
        return;
    }
    navigating_ = true;
    if (state.contains("tabs") && state["tabs"].is_array()) {
        for (const nlohmann::json& entry : state["tabs"]) {
            std::string path = entry.value("path", "");
            std::error_code error;
            if (path.empty() || !fs::is_regular_file(pathFromUtf8(path), error)) {
                continue;
            }
            size_t before = tabs_.size();
            openPath(path);
            if (tabs_.size() == before || !currentTab() || currentTab()->kind != Tab::Kind::Text) {
                continue;
            }
            Tab& tab = *currentTab();
            Position caret{entry.value("line", static_cast<size_t>(0)), entry.value("column", static_cast<size_t>(0))};
            caret = tab.document->buffer().clamp(caret);
            tab.document->selection = {caret, caret};
            tab.scrollY = entry.value("scrollY", 0.0);
            editor_.scrollTo(0, tab.scrollY);
            editor_.selectionDidChange();
        }
    }
    long selected = state.value("selected", -1L);
    if (selected >= 0 && static_cast<size_t>(selected) < tabs_.size()) {
        showTab(selected);
    }
    navigating_ = false;
    if (state.value("terminal", false)) {
        setTerminalVisible(true);
        setFocus(Focus::Editor);
    }
}

void MainWindow::rebuildFileIndex() {
    if (folder_.empty()) {
        fileIndex_.reset();
        return;
    }
    uint64_t generation = ++indexGeneration_;
    std::string folder = folder_;
    std::weak_ptr<bool> alive = alive_;
    runInBackground([this, alive, generation, folder] {
        auto files = std::make_shared<std::vector<std::string>>();
        bool ok = false;
        std::vector<std::string> listed = git::listFiles(folder, &ok);
        if (ok && !listed.empty()) {
            for (std::string& path : listed) {
                std::replace(path.begin(), path.end(), '\\', '/');
                files->push_back(std::move(path));
            }
        } else {
            std::error_code error;
            fs::path root = pathFromUtf8(folder);
            for (auto entry = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error);
                 entry != fs::recursive_directory_iterator() && files->size() < 200000; entry.increment(error)) {
                if (error) {
                    break;
                }
                std::string name = utf8FromPath(entry->path().filename());
                if (entry->is_directory(error)) {
                    if (isNoisyFolder(name) || (!name.empty() && name[0] == '.')) {
                        entry.disable_recursion_pending();
                    }
                    continue;
                }
                files->push_back(genericUtf8(entry->path().lexically_relative(root)));
            }
        }
        runOnMain([this, alive, generation, files] {
            if (!alive.expired() && generation == indexGeneration_) {
                fileIndex_ = files;
            }
        });
    });
}

void MainWindow::quickOpen() {
    std::string folder = folder_;
    auto index = fileIndex_;
    pushOverlay(std::make_unique<PaletteOverlay>(folder.empty() ? "Open a recent file..." : "Go to file...  (name:line)",
                                                 [this, folder, index](const std::string& typed) {
        std::vector<PaletteItem> items;
        std::string query = typed;
        long line = 0;
        size_t colon = query.rfind(':');
        if (colon != std::string::npos && colon + 1 < query.size() &&
            std::all_of(query.begin() + static_cast<long>(colon) + 1, query.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            line = std::stol(query.substr(colon + 1));
            query = query.substr(0, colon);
        }
        query.erase(std::remove(query.begin(), query.end(), ' '), query.end());
        auto itemFor = [this, line](const std::string& absolute, const std::string& shown, const std::string& match) {
            PaletteItem item;
            fs::path path = pathFromUtf8(shown);
            item.title = utf8FromPath(path.filename());
            item.subtitle = genericUtf8(path.parent_path());
            if (!match.empty()) {
                if (auto found = fuzzyMatch(match, item.title)) {
                    item.matched = found->positions;
                }
            }
            item.run = [this, absolute, line] {
                rememberLocation();
                openPath(absolute, line);
            };
            return item;
        };
        if (query.empty() || !index) {
            // Open tabs and recent files first.
            std::set<std::string> seen;
            for (auto& tab : tabs_) {
                std::string path = tab->path();
                if (!path.empty() && seen.insert(pathKey(path)).second) {
                    items.push_back(itemFor(path, relativePath(path), query));
                }
            }
            for (const std::string& path : Services::shared().recentFiles()) {
                std::error_code error;
                if (seen.count(pathKey(path)) || !fs::exists(pathFromUtf8(path), error)) {
                    continue;
                }
                if (!query.empty() && !fuzzyScore(query, path)) {
                    continue;
                }
                seen.insert(pathKey(path));
                items.push_back(itemFor(path, relativePath(path), query));
                if (items.size() >= 30) {
                    break;
                }
            }
            if (!index || query.empty()) {
                return items;
            }
        }
        struct Match {
            int score;
            size_t index;
        };
        std::vector<Match> matches;
        for (size_t position = 0; position < index->size(); ++position) {
            const std::string& path = (*index)[position];
            size_t nameStart = path.rfind('/');
            nameStart = nameStart == std::string::npos ? 0 : nameStart + 1;
            if (auto score = fuzzyScore(query, path, nameStart)) {
                matches.push_back({*score, position});
            }
        }
        size_t shown = std::min<size_t>(matches.size(), 60);
        std::partial_sort(matches.begin(), matches.begin() + static_cast<long>(shown), matches.end(),
                          [](const Match& a, const Match& b) { return a.score > b.score; });
        for (size_t rank = 0; rank < shown; ++rank) {
            const std::string& relative = (*index)[matches[rank].index];
            items.push_back(itemFor(utf8FromPath(pathFromUtf8(folder) / pathFromUtf8(relative)), relative, query));
        }
        return items;
    }));
}

void MainWindow::openRecent() {
    pushOverlay(std::make_unique<PaletteOverlay>("Open a recent folder or file...", [this](const std::string& query) {
        std::vector<PaletteItem> items;
        for (const std::string& folder : Services::shared().recentFolders()) {
            PaletteItem item;
            item.title = utf8FromPath(pathFromUtf8(folder).filename());
            item.subtitle = abbreviateHome(folder);
            if (!paletteMatch(query, item, nullptr)) {
                continue;
            }
            item.run = [this, folder] { openFolder(folder); };
            items.push_back(std::move(item));
        }
        for (const std::string& file : Services::shared().recentFiles()) {
            PaletteItem item;
            item.title = utf8FromPath(pathFromUtf8(file).filename());
            item.subtitle = abbreviateHome(file);
            if (!paletteMatch(query, item, nullptr)) {
                continue;
            }
            item.run = [this, file] { openPath(file); };
            items.push_back(std::move(item));
        }
        return items;
    }));
}

void MainWindow::updateTitle() {
    std::string title;
    if (Tab* tab = currentTab()) {
        title = tab->title() + (tab->dirty() ? " *" : "");
    }
    if (!folder_.empty()) {
        title += (title.empty() ? "" : " - ") + utf8FromPath(pathFromUtf8(folder_).filename());
    }
    title += (title.empty() ? "" : " - ") + std::string("Glasswork Bitmap");
    SDL_SetWindowTitle(window_, title.c_str());
}

// ---------------------------------------------------------------------------
// The side window

void MainWindow::reloadSidePanel() {
    if (!panelOpen_) {
        return;
    }
    switch (panel_) {
        case SidePanel::Files: reloadFiles(); break;
        case SidePanel::Issues: reloadProblems(); break;
        case SidePanel::Debug: debugPanel_->reload(); break;
    }
    dirty_ = true;
}

void MainWindow::reloadFiles() {
    const bitmap::Scheme& colors = Services::shared().scheme();
    Tab* tab = currentTab();
    listedFile_ = tab ? tab->path() : std::string();
    if (folder_.empty()) {
        list_.setRows({{"No folder open.", colors.textDim}, {"File > Open Folder", colors.textDim}});
        return;
    }
    auto inside = [this](const std::string& folder) {
        std::string key = pathKey(folder), root = pathKey(folder_);
        return key == root || (key.size() > root.size() && key.compare(0, root.size(), root) == 0 &&
                               (key[root.size()] == '\\' || key[root.size()] == '/'));
    };
    if (listedFolder_.empty() || !inside(listedFolder_)) {
        listedFolder_ = folder_;
    }
    std::vector<std::pair<std::string, std::string>> folders, files;  // name, path
    std::error_code error;
    for (auto entry = fs::directory_iterator(pathFromUtf8(listedFolder_), fs::directory_options::skip_permission_denied, error);
         entry != fs::directory_iterator(); entry.increment(error)) {
        if (error) {
            break;
        }
        std::string name = utf8FromPath(entry->path().filename());
        if (name.empty() || name[0] == '.') {
            continue;
        }
        std::error_code kind;
        (entry->is_directory(kind) ? folders : files).push_back({name, utf8FromPath(entry->path())});
    }
    auto byName = [](const auto& a, const auto& b) { return naturalLess(a.first, b.first); };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    std::vector<ListRow> rows;
    std::string relative = genericUtf8(pathFromUtf8(listedFolder_).lexically_relative(pathFromUtf8(folder_)));
    std::string header = utf8FromPath(pathFromUtf8(folder_).filename());
    if (header.empty()) {
        header = folder_;
    }
    if (!relative.empty() && relative != ".") {
        header += "/" + relative;
    }
    rows.push_back({header + "/", colors.link});
    if (pathKey(listedFolder_) != pathKey(folder_)) {
        ListRow up{"..", colors.text, true, ui::Icon::Folder, colors.folder};
        up.payload = "..";
        rows.push_back(up);
    }
    for (const auto& [name, path] : folders) {
        ListRow row{name, colors.text, true, ui::Icon::Folder, colors.folder};
        row.payload = "dir:" + path;
        rows.push_back(row);
    }
    std::string current = pathKey(listedFile_);
    for (const auto& [name, path] : files) {
        ListRow row{name, colors.text, true, ui::isImageFile(pathFromUtf8(name)) ? ui::Icon::Image : ui::Icon::File, colors.file};
        row.payload = "file:" + path;
        row.highlighted = pathKey(path) == current;
        rows.push_back(row);
    }
    ListRow add{"new", colors.text, true, ui::Icon::Plus, colors.add};
    add.payload = "+new";
    rows.push_back(add);
    list_.setRows(std::move(rows));
    dirty_ = true;
}

void MainWindow::reloadProblems() {
    const bitmap::Scheme& colors = Services::shared().scheme();
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        list_.setRows({{"No file open.", colors.textDim}});
        return;
    }
    std::vector<Diagnostic> diagnostics = language_->diagnosticsFor(tab->document.get());
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.range.start.line < b.range.start.line; });
    std::vector<ListRow> rows;
    for (const Diagnostic& diagnostic : diagnostics) {
        Color color = diagnostic.severity == 1 ? colors.error : diagnostic.severity == 2 ? colors.warning : colors.info;
        std::string message = diagnostic.message.substr(0, diagnostic.message.find('\n'));
        ListRow row{std::to_string(diagnostic.range.start.line + 1) + " " + message, colors.text, true, ui::Icon::Alert, color};
        row.payload = "issue:" + std::to_string(diagnostic.range.start.line) + ":" + std::to_string(diagnostic.range.start.column);
        rows.push_back(row);
    }
    if (rows.empty()) {
        rows.push_back({"No problems.", colors.good});
    }
    list_.setRows(std::move(rows));
    dirty_ = true;
}

void MainWindow::chooseRow(const ListRow& row) {
    const std::string& payload = row.payload;
    if (payload == "..") {
        listedFolder_ = utf8FromPath(pathFromUtf8(listedFolder_).parent_path());
        reloadFiles();
    } else if (payload == "+new") {
        askForNewFile(false);
    } else if (payload.rfind("dir:", 0) == 0) {
        listedFolder_ = payload.substr(4);
        reloadFiles();
    } else if (payload.rfind("file:", 0) == 0) {
        openPath(payload.substr(5));
        setFocus(Focus::Editor);
    } else if (payload.rfind("issue:", 0) == 0) {
        size_t colon = payload.find(':', 6);
        size_t line = std::stoul(payload.substr(6, colon - 6));
        size_t column = std::stoul(payload.substr(colon + 1));
        rememberLocation();
        editor_.jumpToPosition({line, column});
        setFocus(Focus::Editor);
    }
    dirty_ = true;
}

void MainWindow::fileContextMenu(int rowIndex, int x, int y) {
    if (panel_ != SidePanel::Files || folder_.empty()) {
        return;
    }
    std::string payload = rowIndex >= 0 && static_cast<size_t>(rowIndex) < list_.rows().size() ? list_.rows()[static_cast<size_t>(rowIndex)].payload
                                                                                               : std::string();
    std::string path;
    bool isFolder = false;
    if (payload.rfind("dir:", 0) == 0) {
        path = payload.substr(4);
        isFolder = true;
    } else if (payload.rfind("file:", 0) == 0) {
        path = payload.substr(5);
    }
    Menu menu;
    auto add = [&menu](const std::string& title, std::function<void()> run) {
        MenuItem item;
        item.title = title;
        item.run = std::move(run);
        menu.items.push_back(item);
    };
    if (!path.empty() && !isFolder) {
        add("&Open", [this, path] { openPath(path); });
    }
    if (isFolder) {
        add("&Open", [this, path] {
            listedFolder_ = path;
            reloadFiles();
        });
    }
    add("New &File...", [this] { askForNewFile(false); });
    add("New Fol&der...", [this] { askForNewFile(true); });
    if (!path.empty()) {
        menu.items.push_back(MenuItem::separator());
        add("&Rename...", [this, path] { renamePath(path); });
        add("De&lete", [this, path] { deletePath(path); });
        menu.items.push_back(MenuItem::separator());
        add("Copy &Path", [path] { setClipboardText(path); });
        add("Copy Relati&ve Path", [this, path] { setClipboardText(relativePath(path)); });
    }
    menu.items.push_back(MenuItem::separator());
#ifdef _WIN32
    add("Reveal in &Explorer", [this, path] { revealInFileManager(path.empty() ? listedFolder_ : path); });
#else
    add("Reveal in File &Manager", [this, path] { revealInFileManager(path.empty() ? listedFolder_ : path); });
#endif
    add("Open in &Terminal", [this, path, isFolder] {
        std::string folder = path.empty() ? listedFolder_ : isFolder ? path : utf8FromPath(pathFromUtf8(path).parent_path());
        setTerminalVisible(true);
        terminal_.projectFolder = folder;
        terminal_.newTerminal();
        terminal_.projectFolder = folder_;
        setFocus(Focus::Terminal);
    });
    pushOverlay(std::make_unique<MenuOverlay>(menu, x, y, menuHost()));
}

void MainWindow::askForNewFile(bool folder) {
    if (folder_.empty()) {
        showMessage("New File", "Open a folder first.");
        return;
    }
    DialogOverlay::Options options;
    options.title = folder ? "New Folder" : "New File";
    options.lines = {"In " + abbreviateHome(listedFolder_.empty() ? folder_ : listedFolder_)};
    options.hasField = true;
    options.placeholder = folder ? "folder name" : "file name (a/b.txt makes folders too)";
    options.buttons = {"&Create", "Cancel"};
    std::string base = listedFolder_.empty() ? folder_ : listedFolder_;
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, folder, base](int button, const std::string& text) {
        if (button != 0 || text.empty()) {
            return;
        }
        fs::path target = pathFromUtf8(base) / pathFromUtf8(text);
        std::error_code error;
        if (fs::exists(target, error)) {
            showError("Can't create it", abbreviateHome(utf8FromPath(target)) + " already exists.");
            return;
        }
        if (folder) {
            fs::create_directories(target, error);
        } else {
            fs::create_directories(target.parent_path(), error);
            std::ofstream file(target, std::ios::binary);
            if (!file) {
                showError("Can't create it", "The file couldn't be written.");
                return;
            }
        }
        if (error) {
            showError("Can't create it", error.message());
            return;
        }
        reloadFiles();
        rebuildFileIndex();
        if (!folder) {
            openPath(utf8FromPath(target));
        }
    }));
}

void MainWindow::renamePath(const std::string& path) {
    DialogOverlay::Options options;
    options.title = "Rename";
    options.lines = {"New name for " + utf8FromPath(pathFromUtf8(path).filename()) + ":"};
    options.hasField = true;
    options.fieldText = utf8FromPath(pathFromUtf8(path).filename());
    options.buttons = {"&Rename", "Cancel"};
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, path](int button, const std::string& text) {
        if (button != 0 || text.empty()) {
            return;
        }
        fs::path from = pathFromUtf8(path);
        fs::path to = from.parent_path() / pathFromUtf8(text);
        std::error_code error;
        fs::rename(from, to, error);
        if (error) {
            showError("Couldn't rename it", error.message());
            return;
        }
        std::string fromKey = pathKey(path);
        for (auto& tab : tabs_) {
            std::string tabPath = tab->path();
            if (tabPath.empty()) {
                continue;
            }
            std::string key = pathKey(tabPath);
            if (key == fromKey || (key.size() > fromKey.size() && key.compare(0, fromKey.size(), fromKey) == 0)) {
                fs::path moved = to / pathFromUtf8(tabPath).lexically_relative(from);
                if (key == fromKey) {
                    moved = to;
                }
                tab->document->movedTo(moved.lexically_normal());
                language_->documentDidMove(tab->document.get());
            }
        }
        reloadFiles();
        rebuildFileIndex();
        refresh();
    }));
}

void MainWindow::deletePath(const std::string& path) {
    DialogOverlay::Options options;
    options.title = "Delete";
#ifdef _WIN32
    options.lines = {"Move " + utf8FromPath(pathFromUtf8(path).filename()) + " to the Recycle Bin?"};
#else
    options.lines = {"Move " + utf8FromPath(pathFromUtf8(path).filename()) + " to the Trash?"};
#endif
    options.buttons = {"&Delete", "Cancel"};
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, path](int button, const std::string&) {
        if (button != 0) {
            return;
        }
        std::string error;
        if (!moveToTrash(path, &error)) {
            showError("Couldn't delete it", error);
            return;
        }
        std::string removed = pathKey(path);
        for (long index = static_cast<long>(tabs_.size()) - 1; index >= 0; --index) {
            std::string key = pathKey(tabs_[static_cast<size_t>(index)]->path());
            if (!key.empty() && (key == removed || key.compare(0, removed.size() + 1, removed + "\\") == 0 ||
                                 key.compare(0, removed.size() + 1, removed + "/") == 0)) {
                removeTab(index);
            }
        }
        reloadFiles();
        rebuildFileIndex();
    }));
}

void MainWindow::openSidePanel(SidePanel panel) {
    panelOpen_ = true;
    panel_ = panel;
    Services::shared().setValue("panelOpen", true);
    layout();
    reloadSidePanel();
    setFocus(Focus::Panel);
}

void MainWindow::toggleSidePanel() {
    panelOpen_ = !panelOpen_;
    Services::shared().setValue("panelOpen", panelOpen_);
    if (!panelOpen_ && focus_ == Focus::Panel) {
        setFocus(Focus::Editor);
    }
    layout();
    reloadSidePanel();
}

// ---------------------------------------------------------------------------
// Messages

void MainWindow::showError(const std::string& title, const std::string& message) {
    DialogOverlay::Options options;
    options.title = title;
    options.lines = {message};
    pushOverlay(std::make_unique<DialogOverlay>(options, [](int, const std::string&) {}));
}

void MainWindow::showMessage(const std::string& title, const std::string& message) {
    showError(title, message);
}

void MainWindow::setStatusMessage(const std::string& text, double seconds) {
    temporaryMessage_ = text;
    if (messageTimer_) {
        cancelTimer(messageTimer_);
    }
    std::weak_ptr<bool> alive = alive_;
    messageTimer_ = afterDelay(seconds, [this, alive] {
        if (!alive.expired()) {
            messageTimer_ = 0;
            temporaryMessage_.clear();
            dirty_ = true;
        }
    });
    dirty_ = true;
}

void MainWindow::updateStatus() {
    std::vector<std::string> news;
    size_t errors = language_->errorCount(), warnings = language_->warningCount();
    if (errors > 0) {
        news.push_back(std::to_string(errors) + (errors == 1 ? " error" : " errors"));
    }
    if (warnings > 0) {
        news.push_back(std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings"));
    }
    for (const auto& [token, text] : serverProgress_) {
        news.push_back(text);
    }
    if (!extensionMessage_.empty()) {
        news.push_back(extensionMessage_);
    }
    if (Services::shared().installingDefaults()) {
        news.push_back("Installing language extensions...");
    }
    std::string joined;
    for (const std::string& piece : news) {
        joined += (joined.empty() ? "" : "  ") + piece;
    }
    message_ = !joined.empty() ? joined : currentTab() ? "Ready" : "";
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Version control marks

void MainWindow::refreshGit(Tab& tab) {
    if (tab.kind != Tab::Kind::Text || tab.path().empty() || gitRoot_.empty()) {
        tab.headText.reset();
        tab.hunks.clear();
        if (&tab == currentTab()) {
            editor_.setLineChanges({});
        }
        return;
    }
    std::string relative = genericUtf8(pathFromUtf8(tab.path()).lexically_relative(pathFromUtf8(gitRoot_)));
    if (relative.empty() || relative.rfind("..", 0) == 0) {
        return;
    }
    std::string root = gitRoot_;
    uint64_t identifier = tab.identifier;
    std::weak_ptr<bool> alive = alive_;
    runInBackground([this, alive, root, relative, identifier] {
        auto head = std::make_shared<std::optional<std::string>>(git::readHeadFile(root, relative));
        runOnMain([this, alive, identifier, head] {
            if (alive.expired()) {
                return;
            }
            for (auto& candidate : tabs_) {
                if (candidate->identifier == identifier) {
                    candidate->headText = *head;
                    recomputeDiff(*candidate);
                }
            }
        });
    });
}

void MainWindow::recomputeDiff(Tab& tab) {
    tab.hunks.clear();
    if (tab.headText) {
        std::string text = tab.document->buffer().text();
        tab.hunks = diffLines(splitLines(*tab.headText), splitLines(text));
    }
    if (&tab == currentTab()) {
        editor_.setLineChanges(gutterChanges(tab.hunks));
    }
    dirty_ = true;
}

void MainWindow::scheduleDiff(Tab& tab) {
    if (!tab.headText) {
        return;
    }
    if (tab.diffTimer) {
        cancelTimer(tab.diffTimer);
    }
    uint64_t identifier = tab.identifier;
    std::weak_ptr<bool> alive = alive_;
    tab.diffTimer = afterDelay(0.3, [this, alive, identifier] {
        if (alive.expired()) {
            return;
        }
        for (auto& candidate : tabs_) {
            if (candidate->identifier == identifier) {
                candidate->diffTimer = 0;
                recomputeDiff(*candidate);
            }
        }
    });
}

void MainWindow::changeBarClicked(size_t line, ui::Rect bar) {
    Tab* tab = currentTab();
    if (!tab || !tab->headText) {
        return;
    }
    std::string text = tab->document->buffer().text();
    std::vector<std::string_view> old = splitLines(*tab->headText);
    std::vector<std::string_view> now = splitLines(text);
    for (const DiffHunk& hunk : tab->hunks) {
        size_t end = hunk.newStart + std::max<size_t>(1, hunk.newCount);
        if (line + 1 < hunk.newStart || line >= end + (hunk.newCount == 0 ? 1 : 0)) {
            continue;
        }
        std::vector<std::string> oldLines, newLines;
        for (size_t index = hunk.oldStart; index < hunk.oldStart + hunk.oldCount && index < old.size(); ++index) {
            oldLines.emplace_back(old[index]);
        }
        for (size_t index = hunk.newStart; index < hunk.newStart + hunk.newCount && index < now.size(); ++index) {
            newLines.emplace_back(now[index]);
        }
        changeLine_ = hunk.newStart;
        std::string title = hunk.oldCount == 0 ? "Added" : hunk.newCount == 0 ? "Removed" : "Changed";
        changeBox_.show(title + " (line " + std::to_string(hunk.newStart + 1) + ")", oldLines, newLines, bar, canvas_.bounds());
        dirty_ = true;
        return;
    }
}

void MainWindow::jumpToChange(int direction) {
    Tab* tab = currentTab();
    if (!tab || tab->hunks.empty()) {
        beep();
        return;
    }
    size_t caret = tab->document->selection.caret.line;
    const DiffHunk* target = nullptr;
    if (direction > 0) {
        for (const DiffHunk& hunk : tab->hunks) {
            if (hunk.newStart > caret) {
                target = &hunk;
                break;
            }
        }
        target = target ? target : &tab->hunks.front();
    } else {
        for (auto hunk = tab->hunks.rbegin(); hunk != tab->hunks.rend(); ++hunk) {
            if (hunk->newStart < caret) {
                target = &*hunk;
                break;
            }
        }
        target = target ? target : &tab->hunks.back();
    }
    rememberLocation();
    editor_.jumpToLine(std::min(target->newStart, tab->document->buffer().lineCount() - 1));
}

// ---------------------------------------------------------------------------
// Navigation

void MainWindow::rememberLocation() {
    if (navigating_) {
        return;
    }
    Tab* tab = currentTab();
    if (tab && tab->kind == Tab::Kind::Text && !tab->path().empty()) {
        navigation_.record({tab->path(), tab->document->selection.caret});
    }
}

void MainWindow::navigate(bool back) {
    Tab* tab = currentTab();
    if (!tab || tab->path().empty()) {
        beep();
        return;
    }
    NavigationLocation here{tab->path(), tab->document->selection.caret};
    std::optional<NavigationLocation> target = back ? navigation_.back(here) : navigation_.forward(here);
    if (!target) {
        beep();
        return;
    }
    navigating_ = true;
    openPath(target->path);
    if (Tab* now = currentTab(); now && now->kind == Tab::Kind::Text) {
        editor_.jumpToPosition(now->document->buffer().clamp(target->position));
    }
    navigating_ = false;
}

void MainWindow::goToLine() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return;
    }
    size_t lineCount = tab->document->buffer().lineCount();
    pushOverlay(std::make_unique<PaletteOverlay>("Go to line  (1 - " + std::to_string(lineCount) + ", or line:column)",
                                                 [this, lineCount](const std::string& query) {
        std::vector<PaletteItem> items;
        long line = 0, column = 0;
        try {
            size_t used = 0;
            line = std::stol(query, &used);
            if (used < query.size() && query[used] == ':') {
                column = std::stol(query.substr(used + 1));
            }
        } catch (...) {
            return items;
        }
        if (line < 1) {
            return items;
        }
        line = std::min(line, static_cast<long>(lineCount));
        PaletteItem item;
        item.title = "Go to line " + std::to_string(line) + (column > 0 ? ", column " + std::to_string(column) : "");
        item.run = [this, line, column] {
            rememberLocation();
            if (column > 0) {
                Tab* tab = currentTab();
                if (tab) {
                    editor_.jumpToPosition(tab->document->buffer().clamp({static_cast<size_t>(line - 1), static_cast<size_t>(column - 1)}));
                }
            } else {
                editor_.jumpToLine(static_cast<size_t>(line - 1));
            }
            setFocus(Focus::Editor);
        };
        items.push_back(item);
        return items;
    }));
}

void MainWindow::jumpToSection() {
    std::vector<Section> sections = editor_.sections();
    if (sections.empty()) {
        showMessage("Sections", "This file has no sections. Mark one with a comment such as \"// MARK: Name\", TODO or FIXME.");
        return;
    }
    pushOverlay(std::make_unique<PaletteOverlay>("Go to section...", [this, sections](const std::string& query) {
        std::vector<PaletteItem> items;
        for (const Section& section : sections) {
            PaletteItem item;
            item.title = section.title.empty() ? "-" : section.title;
            const char* kind = section.kind == Section::Kind::Todo ? "TODO" : section.kind == Section::Kind::Fixme ? "FIXME" : "MARK";
            item.subtitle = std::string(kind) + " - line " + std::to_string(section.line + 1);
            if (!paletteMatch(query, item, nullptr)) {
                continue;
            }
            size_t line = section.line;
            item.run = [this, line] {
                rememberLocation();
                editor_.jumpToLine(line);
                setFocus(Focus::Editor);
            };
            items.push_back(std::move(item));
        }
        return items;
    }));
}

std::string MainWindow::relativePath(const std::string& path) const {
    if (!folder_.empty()) {
        std::string key = pathKey(path), root = pathKey(folder_);
        if (key.size() > root.size() && key.compare(0, root.size(), root) == 0) {
            return genericUtf8(pathFromUtf8(path).lexically_relative(pathFromUtf8(folder_)));
        }
    }
    return abbreviateHome(path);
}

void MainWindow::applyEditorConfig(Tab& tab) {
    if (tab.kind != Tab::Kind::Text || editor_.document() != tab.document.get()) {
        return;
    }
    const EditorConfigResult& config = tab.editorConfig;
    int usesTabs = config.useTabs ? (*config.useTabs ? 1 : 0) : -1;
    size_t size = config.columnsPerIndent() ? static_cast<size_t>(std::max(1, *config.columnsPerIndent())) : 0;
    editor_.setIndentationOverride(usesTabs, size);
}

// ---------------------------------------------------------------------------
// Extensions

void MainWindow::manageExtensions() {
    pushOverlay(std::make_unique<PaletteOverlay>("Installed extensions... (choose one to manage)", [this](const std::string& query) {
        std::vector<PaletteItem> items;
        PaletteItem install;
        install.title = "Install extensions from Open VSX...";
        install.run = [this] { installExtensions(); };
        if (paletteMatch(query, install, nullptr)) {
            items.push_back(install);
        }
        auto add = [&](const Extension& extension, bool disabled) {
            PaletteItem item;
            item.title = extension.displayName.empty() ? extension.id : extension.displayName;
            item.subtitle = extension.id + " " + extension.version + (disabled ? "  (disabled)" : "");
            if (!paletteMatch(query, item, nullptr)) {
                return;
            }
            std::string id = extension.id, name = item.title;
            item.run = [this, id, name, disabled] {
                DialogOverlay::Options options;
                options.title = name;
                options.lines = {id};
                options.buttons = {disabled ? "&Enable" : "&Disable", "&Uninstall", "Cancel"};
                pushOverlay(std::make_unique<DialogOverlay>(options, [this, id, disabled](int button, const std::string&) {
                    if (button == 0) {
                        Services::shared().setExtensionEnabled(id, disabled);
                        setStatusMessage(std::string(disabled ? "Enabled " : "Disabled ") + id);
                    } else if (button == 1) {
                        Services::shared().uninstallExtension(id);
                        setStatusMessage("Uninstalled " + id);
                    }
                }));
            };
            items.push_back(std::move(item));
        };
        for (const Extension& extension : Services::shared().extensions().extensions()) {
            add(extension, false);
        }
        for (const Extension& extension : Services::shared().extensions().disabledExtensions()) {
            add(extension, true);
        }
        return items;
    }));
}

void MainWindow::installExtensions() {
    struct Search {
        std::string query = "\x01";
        std::vector<ExtensionManager::MarketplaceEntry> results;
        std::string error;
        uint64_t token = 0;
        bool searching = false;
    };
    auto search = std::make_shared<Search>();
    auto holder = std::make_shared<PaletteOverlay*>(nullptr);
    std::weak_ptr<bool> alive = alive_;
    auto provider = [this, alive, search, holder](const std::string& query) {
        if (query != search->query) {
            search->query = query;
            uint64_t token = ++search->token;
            search->searching = true;
            afterDelay(0.3, [this, alive, search, holder, token, query] {
                if (alive.expired() || token != search->token) {
                    return;
                }
                runInBackground([this, alive, search, holder, token, query] {
                    std::string error;
                    auto results = ExtensionManager::searchOpenVSX(query, 40, &error);
                    runOnMain([this, alive, search, holder, token, results, error] {
                        if (alive.expired() || token != search->token) {
                            return;
                        }
                        search->results = results;
                        search->error = error;
                        search->searching = false;
                        for (auto& candidate : overlays_) {
                            if (candidate.get() == *holder && !candidate->closed()) {
                                (*holder)->refresh();
                                dirty_ = true;
                            }
                        }
                    });
                });
            });
        }
        std::vector<PaletteItem> items;
        if (!search->error.empty()) {
            PaletteItem item;
            item.title = "Search failed";
            item.subtitle = search->error;
            items.push_back(item);
        }
        for (const auto& entry : search->results) {
            PaletteItem item;
            bool installed = Services::shared().extensions().isInstalled(entry.id);
            item.title = (entry.displayName.empty() ? entry.id : entry.displayName) + (installed ? "  (installed)" : "");
            item.subtitle = entry.id + " - " + entry.description;
            std::string id = entry.id;
            item.run = [this, alive, id] {
                setStatusMessage("Installing " + id + "...", 60);
                Services::shared().installExtension(id, [this, alive, id](bool ok, std::string message) {
                    if (alive.expired()) {
                        return;
                    }
                    setStatusMessage(ok ? "Installed " + id : "Couldn't install " + id + ": " + message, 6);
                });
            };
            items.push_back(std::move(item));
        }
        if (items.empty()) {
            PaletteItem item;
            item.title = search->searching ? "Searching..." : "Nothing found";
            items.push_back(item);
        }
        return items;
    };
    auto palette = std::make_unique<PaletteOverlay>("Search Open VSX for extensions...", provider);
    *holder = palette.get();
    pushOverlay(std::move(palette));
}

}  // namespace gw::app
