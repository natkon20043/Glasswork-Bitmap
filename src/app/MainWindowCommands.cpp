#include "MainWindow.hpp"

#include "App.hpp"
#include "BitmapMode.hpp"
#include "Font.hpp"
#include "Loop.hpp"
#include "Paths.hpp"
#include "Platform.hpp"
#include "Services.hpp"

#include <algorithm>

#ifndef GLASSWORK_VERSION
#define GLASSWORK_VERSION "1.0.0"
#endif

namespace gw::app {

namespace {

bool isFunctionKey(const std::string& key) {
    return key.size() >= 2 && key[0] == 'f' && std::isdigit(static_cast<unsigned char>(key[1]));
}

/// Commands that keep working while the terminal has the keyboard; every other key goes to the shell.
const std::set<std::string>& commandsSkippingShell() {
    static const std::set<std::string> commands = {
        "workbench.action.showCommands",       "workbench.action.quickOpen",       "workbench.action.terminal.toggleTerminal",
        "workbench.action.terminal.new",       "workbench.action.files.save",      "workbench.action.files.saveAll",
        "workbench.action.newWindow",          "workbench.action.closeWindow",     "workbench.action.quit",
        "workbench.action.debug.start",        "workbench.action.debug.stop",      "workbench.action.debug.stepOver",
        "workbench.action.debug.stepInto",     "workbench.action.debug.stepOut",   "workbench.action.nextEditor",
        "workbench.action.previousEditor",     "workbench.action.zoomIn",          "workbench.action.zoomOut",
        "workbench.action.zoomReset",          "workbench.view.explorer",          "workbench.view.debug",
        "workbench.actions.view.problems",     "workbench.action.tasks.build",     "workbench.action.tasks.runTask",
        "glasswork.runCurrentFile",            "workbench.action.openSettings",    "workbench.action.files.openFolder",
        "workbench.extensions.search",         "workbench.action.files.openFile",  "glasswork.openFolderInNewWindow",
    };
    return commands;
}

}  // namespace

// ---------------------------------------------------------------------------
// The command list

void MainWindow::addCommand(const std::string& id, const std::string& title, std::function<void()> run, std::function<bool()> enabled,
                            bool hidden) {
    Command command;
    command.id = id;
    command.title = title;
    command.run = [run](const nlohmann::json&) { run(); };
    command.enabled = std::move(enabled);
    command.hidden = hidden;
    if (commands_.count(id) == 0) {
        commandOrder_.push_back(id);
    }
    commands_[id] = std::move(command);
}

void MainWindow::registerCommands() {
    auto hasText = [this] { return currentTab() && currentTab()->kind == Tab::Kind::Text; };
    auto hasFile = [this] { return currentTab() != nullptr; };
    auto editable = [this] { return currentTab() && currentTab()->kind == Tab::Kind::Text && !currentTab()->document->readOnly; };
    auto inEditor = [this](std::function<void()> work) {
        return [this, work] {
            if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
                work();
                dirty_ = true;
            }
        };
    };

    // ---- Files
    addCommand("workbench.action.files.newUntitledFile", "File: New File", [this] { newFile(); });
    addCommand("workbench.action.files.openFile", "File: Open File...", [this] { openFileDialog(); });
    addCommand("workbench.action.files.openFolder", "File: Open Folder...", [this] { openFolderDialog(false); });
    addCommand("glasswork.openFolderInNewWindow", "File: Open Folder in New Window...", [this] { openFolderDialog(true); });
    addCommand("workbench.action.openRecent", "File: Open Recent...", [this] { openRecent(); });
    addCommand("workbench.action.quickOpen", "Go to File...", [this] { quickOpen(); });
    addCommand("workbench.action.files.save", "File: Save", [this] {
        if (Tab* tab = currentTab()) {
            saveTab(*tab);
        }
    }, hasText);
    addCommand("workbench.action.files.saveAs", "File: Save As...", [this] {
        if (Tab* tab = currentTab()) {
            saveTabAs(*tab);
        }
    }, hasText);
    addCommand("workbench.action.files.saveAll", "File: Save All", [this] { saveAll(); });
    addCommand("workbench.action.closeActiveEditor", "View: Close Editor", [this] {
        if (selected_ >= 0) {
            closeTab(selected_);
        }
    }, hasFile);
    addCommand("workbench.action.closeOtherEditors", "View: Close Other Editors", [this] { closeOtherTabs(); }, hasFile);
    addCommand("workbench.action.reopenClosedEditor", "View: Reopen Closed Editor", [this] { reopenClosedTab(); },
               [this] { return !closedPaths_.empty(); });
    addCommand("workbench.action.newWindow", "New Window", [this] { app_.openWindow("", {}); });
    addCommand("workbench.action.closeWindow", "Close Window", [this] {
        requestClose([this](bool ok) {
            if (ok) {
                app_.closeWindow(this);
            }
        });
    });
    addCommand("workbench.action.quit", "Quit", [this] { app_.quit(); });
    addCommand("workbench.action.nextEditor", "View: Open Next Editor", [this] {
        if (!tabs_.empty()) {
            showTab((selected_ + 1) % static_cast<long>(tabs_.size()));
        }
    }, hasFile);
    addCommand("workbench.action.previousEditor", "View: Open Previous Editor", [this] {
        if (!tabs_.empty()) {
            showTab((selected_ + static_cast<long>(tabs_.size()) - 1) % static_cast<long>(tabs_.size()));
        }
    }, hasFile);
    addCommand("workbench.action.openSettings", "Preferences: Open Settings (JSON)", [this] { openSettingsFile(); });
    addCommand("glasswork.openKeybindings", "Preferences: Open Keyboard Shortcuts (JSON)", [this] { openKeybindingsFile(); });
    addCommand("markdown.showPreview", "Markdown: Open Preview", [this] { showPreview(); },
               [this] {
                   Tab* tab = currentTab();
                   if (!tab || tab->kind != Tab::Kind::Text) {
                       return false;
                   }
                   std::string extension = utf8FromPath(pathFromUtf8(tab->path()).extension());
                   std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                   return tab->document->languageId == "markdown" || extension == ".md" || extension == ".markdown";
               });
    addCommand("workbench.action.files.revealActiveFileInWindows", "File: Reveal in File Manager", [this] {
        if (Tab* tab = currentTab(); tab && !tab->path().empty()) {
            revealInFileManager(tab->path());
        }
    }, [this] { return currentTab() && !currentTab()->path().empty(); });
    addCommand("copyFilePath", "File: Copy Path of Active File", [this] {
        if (Tab* tab = currentTab(); tab && !tab->path().empty()) {
            setClipboardText(tab->path());
        }
    }, [this] { return currentTab() && !currentTab()->path().empty(); });

    // ---- Editing
    addCommand("undo", "Undo", inEditor([this] { editor_.undo(); }), [this] { return currentTab() && currentTab()->kind == Tab::Kind::Text && editor_.canUndo(); });
    addCommand("redo", "Redo", inEditor([this] { editor_.redo(); }), [this] { return currentTab() && currentTab()->kind == Tab::Kind::Text && editor_.canRedo(); });
    addCommand("editor.action.clipboardCutAction", "Cut", [this] {
        if (focus_ == Focus::Terminal) {
            terminal_.copySelection();
        } else if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
            editor_.cut();
        }
    }, editable);
    addCommand("editor.action.clipboardCopyAction", "Copy", [this] {
        if (focus_ == Focus::Terminal) {
            terminal_.copySelection();
        } else if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
            editor_.copy();
        }
    });
    addCommand("editor.action.clipboardPasteAction", "Paste", [this] {
        if (focus_ == Focus::Terminal) {
            terminal_.paste();
        } else if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
            editor_.paste();
        }
    });
    addCommand("editor.action.selectAll", "Select All", [this] {
        if (focus_ == Focus::Terminal) {
            terminal_.selectAll();
        } else if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
            editor_.selectAll();
        }
    });
    addCommand("editor.action.commentLine", "Toggle Line Comment", inEditor([this] { editor_.toggleLineComment(); }), editable);
    addCommand("editor.action.indentLines", "Indent Line", inEditor([this] { editor_.indentLines(); }), editable);
    addCommand("editor.action.outdentLines", "Outdent Line", inEditor([this] { editor_.outdentLines(); }), editable);
    addCommand("editor.action.moveLinesUpAction", "Move Line Up", inEditor([this] { editor_.moveLinesUp(); }), editable);
    addCommand("editor.action.moveLinesDownAction", "Move Line Down", inEditor([this] { editor_.moveLinesDown(); }), editable);
    addCommand("editor.action.copyLinesUpAction", "Copy Line Up", inEditor([this] { editor_.duplicateLinesUp(); }), editable);
    addCommand("editor.action.copyLinesDownAction", "Copy Line Down", inEditor([this] { editor_.duplicateLinesDown(); }), editable);
    addCommand("editor.action.deleteLines", "Delete Line", inEditor([this] { editor_.deleteLines(); }), editable);
    addCommand("editor.action.joinLines", "Join Lines", inEditor([this] { editor_.joinSelectedLines(); }), editable);
    addCommand("editor.action.insertLineAfter", "Insert Line Below", inEditor([this] { editor_.insertLineBelow(); }), editable);
    addCommand("editor.action.insertLineBefore", "Insert Line Above", inEditor([this] { editor_.insertLineAbove(); }), editable);
    addCommand("editor.action.smartSelect.expand", "Expand Selection", inEditor([this] { editor_.expandSelection(); }), hasText);
    addCommand("editor.action.smartSelect.shrink", "Shrink Selection", inEditor([this] { editor_.shrinkSelection(); }), hasText);
    addCommand("editor.action.jumpToBracket", "Go to Bracket", inEditor([this] { editor_.goToMatchingBracket(); }), hasText);
    addCommand("editor.action.addSelectionToNextFindMatch", "Add Selection to Next Find Match",
               inEditor([this] { editor_.selectNextOccurrence(); }), hasText);
    addCommand("editor.action.selectHighlights", "Select All Occurrences of Find Match",
               inEditor([this] { editor_.selectAllOccurrences(); }), hasText);
    addCommand("editor.action.insertCursorAbove", "Add Cursor Above", inEditor([this] { editor_.addCursorAbove(); }), hasText);
    addCommand("editor.action.insertCursorBelow", "Add Cursor Below", inEditor([this] { editor_.addCursorBelow(); }), hasText);
    addCommand("editor.action.transformToUppercase", "Transform to Uppercase", inEditor([this] { editor_.makeUpperCase(); }), editable);
    addCommand("editor.action.transformToLowercase", "Transform to Lowercase", inEditor([this] { editor_.makeLowerCase(); }), editable);
    addCommand("editor.action.transformToTitlecase", "Transform to Title Case", inEditor([this] { editor_.makeTitleCase(); }), editable);
    addCommand("editor.action.sortLinesAscending", "Sort Lines Ascending", inEditor([this] { editor_.sortLinesAscending(); }), editable);
    addCommand("editor.action.sortLinesDescending", "Sort Lines Descending", inEditor([this] { editor_.sortLinesDescending(); }), editable);
    addCommand("editor.action.removeDuplicateLines", "Delete Duplicate Lines", inEditor([this] { editor_.removeDuplicateLines(); }), editable);
    addCommand("editor.fold", "Fold", inEditor([this] { editor_.foldCode(); }), hasText);
    addCommand("editor.unfold", "Unfold", inEditor([this] { editor_.unfoldCode(); }), hasText);
    addCommand("editor.foldAll", "Fold All", inEditor([this] { editor_.foldAllCode(); }), hasText);
    addCommand("editor.unfoldAll", "Unfold All", inEditor([this] { editor_.unfoldAllCode(); }), hasText);
    addCommand("editor.action.toggleWordWrap", "View: Toggle Word Wrap", [] {
        Services::shared().updateSettings([](EditorSettings& settings) { settings.wordWrap = !settings.wordWrap; });
    });

    // ---- Search and navigation
    addCommand("actions.find", "Find", [this] {
        if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
            findBar_->show(editor_.selectedText(), false);
            setFocus(Focus::Find);
            layout();
        }
    }, hasText);
    addCommand("editor.action.startFindReplace", "Replace", [this] {
        if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
            findBar_->show(editor_.selectedText(), true);
            setFocus(Focus::Find);
            layout();
        }
    }, editable);
    addCommand("editor.action.nextMatchFindAction", "Find Next", [this] { findBar_->findNext(); }, hasText);
    addCommand("editor.action.previousMatchFindAction", "Find Previous", [this] { findBar_->findPrevious(); }, hasText);
    addCommand("glasswork.useSelectionForFind", "Use Selection for Find", [this] {
        std::string selected = editor_.selectedText();
        if (!selected.empty()) {
            findBar_->setSearchText(selected);
        }
    }, hasText);
    addCommand("workbench.action.gotoLine", "Go to Line...", [this] { goToLine(); }, hasText);
    addCommand("glasswork.jumpToSection", "Go to Section...", [this] { jumpToSection(); }, hasText);
    addCommand("workbench.action.navigateBack", "Go Back", [this] { navigate(true); });
    addCommand("workbench.action.navigateForward", "Go Forward", [this] { navigate(false); });
    addCommand("workbench.action.editor.nextChange", "Go to Next Change", [this] { jumpToChange(1); }, hasText);
    addCommand("workbench.action.editor.previousChange", "Go to Previous Change", [this] { jumpToChange(-1); }, hasText);

    // ---- Code intelligence
    addCommand("editor.action.triggerSuggest", "Trigger Suggest", [this] { requestSuggestions(true); }, editable);
    addCommand("editor.action.revealDefinition", "Go to Definition",
               [this] { requestLocations("textDocument/definition", "Definitions...", true); }, hasText);
    addCommand("editor.action.goToReferences", "Find All References",
               [this] { requestLocations("textDocument/references", "References...", false); }, hasText);
    addCommand("workbench.action.gotoSymbol", "Go to Symbol in Editor...", [this] { goToSymbolInFile(); }, hasText);
    addCommand("workbench.action.showAllSymbols", "Go to Symbol in Workspace...", [this] { goToSymbolInWorkspace(); }, hasText);
    addCommand("editor.action.quickFix", "Quick Fix...", [this] { showQuickFixes(); }, editable);
    addCommand("editor.action.rename", "Rename Symbol", [this] { renameSymbol(); }, editable);
    addCommand("editor.action.formatDocument", "Format Document", [this] {
        if (Document* document = documentForLanguageCommand()) {
            if (!language_->serverSupports("documentFormattingProvider", document)) {
                showMessage("Code Intelligence", "The language server for this file can't format it.");
                return;
            }
            formatDocument(document, nullptr);
        }
    }, editable);
    addCommand("workbench.actions.view.problems", "View: Show Problems", [this] { openSidePanel(SidePanel::Issues); });
    addCommand("glasswork.problemsInFile", "Problems in This File...", [this] { showProblemsList(); }, hasText);
    addCommand("glasswork.restartExtensionHost", "Developer: Restart Extension Host", [this] { extensionHost_->restart(); });

    // ---- Views
    addCommand("workbench.view.explorer", "View: Show Files", [this] { openSidePanel(SidePanel::Files); });
    addCommand("workbench.view.debug", "View: Show Debugger", [this] { openSidePanel(SidePanel::Debug); });
    addCommand("workbench.action.toggleSidebarVisibility", "View: Toggle Side Window", [this] { toggleSidePanel(); });
    addCommand("workbench.action.zoomIn", "View: Bigger Text", [this] { zoom(1); });
    addCommand("workbench.action.zoomOut", "View: Smaller Text", [this] { zoom(-1); });
    addCommand("workbench.action.zoomReset", "View: Reset Text Size", [] {
        Services::shared().updateSettings([](EditorSettings& settings) { settings.fontSize = 13; });
    });
    addCommand("workbench.action.showCommands", "Show All Commands", [this] { showCommandPalette(); });
    addCommand("glasswork.openMenuBar", "View: Open Menu Bar", [this] { openMenu(1); });
    addCommand("glasswork.showKeys", "Help: Keys", [this] { showKeys(); });
    addCommand("glasswork.about", "Help: About Glasswork Bitmap", [this] { showAbout(); });
    addCommand("glasswork.chooseFont", "Options: Other Font...", [this] { chooseFont(); });
    addCommand("glasswork.usePixelFont", "Options: Pixel Font", [] {
        Services::shared().updateSettings([](EditorSettings& settings) { settings.bitmapFont.clear(); });
    });
    addCommand("glasswork.toggleCursorSmear", "Options: Cursor Smear", [] {
        Services::shared().updateSettings([](EditorSettings& settings) { settings.vimCursorSmear = !settings.vimCursorSmear; });
    });
    const auto& schemes = bitmap::schemes();
    for (const bitmap::Scheme& scheme : schemes) {
        std::string name = scheme.name;
        addCommand("glasswork.colors." + name, "Options: Colors " + name, [name] {
            Services::shared().updateSettings([name](EditorSettings& settings) { settings.bitmapColors = name; });
        });
    }
    addCommand("workbench.extensions.search", "Extensions: Install Extensions...", [this] { installExtensions(); });
    addCommand("workbench.extensions.action.showInstalledExtensions", "Extensions: Show Installed Extensions",
               [this] { manageExtensions(); });

    // ---- Run and debug
    addCommand("workbench.action.terminal.toggleTerminal", "View: Toggle Terminal", [this] {
        if (terminalVisible_ && focus_ != Focus::Terminal) {
            setFocus(Focus::Terminal);
            return;
        }
        setTerminalVisible(!terminalVisible_);
    });
    addCommand("workbench.action.terminal.new", "Terminal: Create New Terminal", [this] {
        setTerminalVisible(true);
        terminal_.newTerminal();
        setFocus(Focus::Terminal);
    });
    addCommand("workbench.action.terminal.kill", "Terminal: Kill the Active Terminal", [this] { terminal_.closeCurrentTerminal(); },
               [this] { return terminalVisible_; });
    addCommand("workbench.action.terminal.clear", "Terminal: Clear", [this] { terminal_.clear(); }, [this] { return terminalVisible_; });
    addCommand("workbench.action.tasks.runTask", "Tasks: Run Task...", [this] { runTask(); });
    addCommand("workbench.action.tasks.build", "Tasks: Run Build Task", [this] { buildProject(); });
    addCommand("glasswork.runCurrentFile", "Run: Run File", [this] { runCurrentFile(); }, hasText);
    addCommand("workbench.action.debug.start", "Debug: Start / Continue", [this] { startDebugging(); });
    addCommand("glasswork.debugCurrentFile", "Debug: Debug This File", [this] { debugCurrentFile(); }, hasText);
    addCommand("glasswork.debugExecutable", "Debug: Debug Executable...", [this] { debugExecutable(); });
    addCommand("workbench.action.debug.stop", "Debug: Stop", [this] { debugger_.stop(); },
               [this] { return debugger_.state() != DebugState::Idle; });
    addCommand("workbench.action.debug.pause", "Debug: Pause", [this] { debugger_.pause(); },
               [this] { return debugger_.state() == DebugState::Running; });
    addCommand("workbench.action.debug.stepOver", "Debug: Step Over", [this] { debugger_.stepOver(); },
               [this] { return debugger_.state() == DebugState::Stopped; });
    addCommand("workbench.action.debug.stepInto", "Debug: Step Into", [this] { debugger_.stepInto(); },
               [this] { return debugger_.state() == DebugState::Stopped; });
    addCommand("workbench.action.debug.stepOut", "Debug: Step Out", [this] { debugger_.stepOut(); },
               [this] { return debugger_.state() == DebugState::Stopped; });
    addCommand("editor.debug.action.toggleBreakpoint", "Debug: Toggle Breakpoint", [this] { toggleBreakpoint(); }, hasText);
    addCommand("workbench.debug.viewlet.action.removeAllBreakpoints", "Debug: Remove All Breakpoints", [this] { removeAllBreakpoints(); });
}

bool MainWindow::runCommand(const std::string& id, const nlohmann::json& arguments) {
    auto found = commands_.find(id);
    if (found == commands_.end()) {
        if (extensionHost_ && !id.empty()) {
            extensionHost_->executeCommand(id, arguments.is_null() ? Json::array() : arguments);
            return true;
        }
        return false;
    }
    const Command& command = found->second;
    if (command.enabled && !command.enabled()) {
        beep();
        return true;
    }
    command.run(arguments);
    dirty_ = true;
    return true;
}

std::map<std::string, std::string> MainWindow::keyContext() const {
    std::map<std::string, std::string> context = extensionContext_;
    Tab* tab = currentTab();
    bool text = tab && tab->kind == Tab::Kind::Text;
    bool overlay = topOverlay() != nullptr;
    auto flag = [](bool value) { return value ? std::string("true") : std::string("false"); };
    context["editorTextFocus"] = flag(text && focus_ == Focus::Editor && !overlay);
    context["editorFocus"] = flag(text && focus_ == Focus::Editor && !overlay);
    context["terminalFocus"] = flag(focus_ == Focus::Terminal && !overlay);
    context["inputFocus"] = flag(overlay || focus_ == Focus::Find || (focus_ == Focus::Panel && debugPanel_->wantsKeys()));
    context["findWidgetVisible"] = flag(findBar_->visible());
    context["suggestWidgetVisible"] = flag(suggestions_.visible());
    context["sideBarVisible"] = flag(panelOpen_);
    context["terminalIsOpen"] = flag(terminalVisible_);
    context["inDebugMode"] = flag(debugger_.state() != DebugState::Idle);
    context["vim.active"] = "true";
#ifdef _WIN32
    context["isWindows"] = "true";
#else
    context["isLinux"] = "true";
#endif
    if (text) {
        const Document& document = *tab->document;
        context["editorLangId"] = document.languageId;
        std::string path = tab->path();
        if (!path.empty()) {
            std::filesystem::path file = pathFromUtf8(path);
            context["resourceExtname"] = utf8FromPath(file.extension());
            context["resourceFilename"] = utf8FromPath(file.filename());
            context["resourceLangId"] = document.languageId;
        }
        context["editorHasSelection"] = flag(!document.selection.empty());
        context["editorHasMultipleSelections"] = flag(!document.extraSelections.empty());
        context["editorReadonly"] = flag(document.readOnly);
    }
    return context;
}

std::string MainWindow::shortcutForCommand(const std::string& id) const {
    std::map<std::string, std::string> context = keyContext();
    context["editorTextFocus"] = "true";
    context["editorFocus"] = "true";
    context["terminalFocus"] = "false";
    context["inputFocus"] = "false";
    std::vector<KeyChord> chords = Services::shared().keybindings().shortcutFor(id, context);
    return chords.empty() ? std::string() : shortcutText(chords);
}

bool MainWindow::dispatchKey(const KeyPress& key) {
    Tab* tab = currentTab();
    bool editorText = focus_ == Focus::Editor && tab && tab->kind == Tab::Kind::Text;

    // Vim's own Ctrl keys come before the shortcuts: Ctrl-[ always, Ctrl-C and Ctrl-R outside insert mode.
    if (editorText && key.control && !key.alt && !key.shift && !key.meta &&
        (key.key == "[" || (!editor_.insertsText() && (key.key == "c" || key.key == "r")))) {
        if (editor_.keyDown(key)) {
            return true;
        }
    }

    bool tryShortcuts = !pendingChords_.empty() || key.control || key.alt || key.meta || isFunctionKey(key.key) ||
                        (key.shift && (key.key == "delete" || key.key == "insert"));
    if (tryShortcuts) {
        std::vector<KeyChord> pressed = pendingChords_;
        pressed.push_back(key.chord());
        std::map<std::string, std::string> context = keyContext();
        KeybindingTable::Match match = Services::shared().keybindings().match(pressed, context);
        if (match.kind == KeybindingTable::MatchKind::Prefix) {
            pendingChords_ = pressed;
            setStatusMessage("(" + shortcutText(pressed) + ") was pressed. Waiting for the next key...", 3);
            return true;
        }
        bool wasPending = !pendingChords_.empty();
        pendingChords_.clear();
        if (match.kind == KeybindingTable::MatchKind::Complete && match.binding) {
            const KeyBinding& binding = *match.binding;
            bool allowed = focus_ != Focus::Terminal || commandsSkippingShell().count(binding.command) > 0 ||
                           binding.when.find("terminalFocus") != std::string::npos;
            if (allowed) {
                temporaryMessage_.clear();
                runCommand(binding.command, binding.arguments);
                return true;
            }
        } else if (wasPending) {
            setStatusMessage("The key combination (" + shortcutText(pressed) + ") is not a command.", 3);
            return true;
        }
    }

    bool used = false;
    switch (focus_) {
        case Focus::Editor:
            if (editorText) {
                if (key.plain() && key.key == "escape" && findBar_->visible() && !Services::shared().settings().vimMode) {
                    findBar_->close();
                    used = true;
                } else {
                    used = editor_.keyDown(key);
                }
                updateTextInput();
            } else if (tab && tab->kind == Tab::Kind::Preview) {
                if (key.key == "down" || key.key == "j") {
                    tab->scrollY += 1;
                    used = true;
                } else if (key.key == "up" || key.key == "k") {
                    tab->scrollY = std::max(0.0, tab->scrollY - 1);
                    used = true;
                } else if (key.key == "pagedown" || key.key == "space") {
                    tab->scrollY += 20;
                    used = true;
                } else if (key.key == "pageup") {
                    tab->scrollY = std::max(0.0, tab->scrollY - 20);
                    used = true;
                }
            }
            break;
        case Focus::Terminal:
            used = terminal_.keyDown(key);
            break;
        case Focus::Find:
            used = findBar_->keyDown(key);
            if (!used && key.plain() && key.key == "escape") {
                findBar_->close();
                used = true;
            }
            break;
        case Focus::Panel:
            if (panel_ == SidePanel::Debug && debugPanel_->wantsKeys()) {
                used = debugPanel_->keyDown(key);
            } else if (panel_ == SidePanel::Debug) {
                used = debugPanel_->keyDown(key);
            } else {
                used = list_.keyDown(key);
            }
            if (!used && key.plain() && key.key == "escape") {
                setFocus(Focus::Editor);
                used = true;
            }
            break;
    }
    if (used) {
        return key.control || key.alt || key.meta;
    }
    // Alt and a menu's letter opens that menu (not in the terminal, where Alt is the shell's).
    if (key.alt && !key.control && !key.meta && key.key.size() == 1 && focus_ != Focus::Terminal) {
        static const char hotkeys[] = {0, 'f', 'e', 's', 'r', 'd', 'v', 'o', 'h'};
        for (int index = 1; index < 9; ++index) {
            if (hotkeys[index] == key.key[0]) {
                openMenu(index);
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Menus

std::vector<Menu> MainWindow::buildMenus() {
    auto item = [](const char* title, const char* command) {
        MenuItem entry;
        entry.title = title;
        entry.command = command;
        return entry;
    };
    MenuItem line = MenuItem::separator();
    std::vector<Menu> menus;
    menus.push_back({"≡", {item("&About Glasswork Bitmap", "glasswork.about"), item("&New Window", "workbench.action.newWindow"), line,
                           item("&Settings...", "workbench.action.openSettings"), item("&Quit", "workbench.action.quit")}});
    menus.push_back({"&File",
                     {item("&New", "workbench.action.files.newUntitledFile"), item("&Open...", "workbench.action.files.openFile"),
                      item("Open &Folder...", "workbench.action.files.openFolder"), item("Open Rec&ent...", "workbench.action.openRecent"),
                      item("&Quick Open...", "workbench.action.quickOpen"), line, item("&Save", "workbench.action.files.save"),
                      item("Save &As...", "workbench.action.files.saveAs"), item("Save A&ll", "workbench.action.files.saveAll"), line,
                      item("&Close Tab", "workbench.action.closeActiveEditor"), item("&Reopen Tab", "workbench.action.reopenClosedEditor")}});
    menus.push_back({"&Edit",
                     {item("&Undo", "undo"), item("&Redo", "redo"), line, item("Cu&t", "editor.action.clipboardCutAction"),
                      item("&Copy", "editor.action.clipboardCopyAction"), item("&Paste", "editor.action.clipboardPasteAction"),
                      item("Select &All", "editor.action.selectAll"), line, item("Toggle Co&mment", "editor.action.commentLine")}});
    menus.push_back({"&Search",
                     {item("&Find...", "actions.find"), item("&Replace...", "editor.action.startFindReplace"),
                      item("Find &Next", "editor.action.nextMatchFindAction"), item("Find &Previous", "editor.action.previousMatchFindAction"),
                      line, item("&Go to Line...", "workbench.action.gotoLine"), item("&Symbol...", "workbench.action.gotoSymbol"),
                      item("&Definition", "editor.action.revealDefinition")}});
    menus.push_back({"&Run",
                     {item("&Run File", "glasswork.runCurrentFile"), item("&Build", "workbench.action.tasks.build"),
                      item("Run &Task...", "workbench.action.tasks.runTask"), line,
                      item("T&erminal", "workbench.action.terminal.toggleTerminal")}});
    menus.push_back({"&Debug",
                     {item("&Start / Continue", "workbench.action.debug.start"), item("S&top", "workbench.action.debug.stop"), line,
                      item("Step &Over", "workbench.action.debug.stepOver"), item("Step &Into", "workbench.action.debug.stepInto"),
                      item("Step O&ut", "workbench.action.debug.stepOut"), line,
                      item("Toggle &Breakpoint", "editor.debug.action.toggleBreakpoint"),
                      item("&Clear Breakpoints", "workbench.debug.viewlet.action.removeAllBreakpoints"), line,
                      item("Debug &Executable...", "glasswork.debugExecutable")}});
    MenuItem wrap = item("&Word Wrap", "editor.action.toggleWordWrap");
    wrap.checked = [] { return Services::shared().settings().wordWrap; };
    MenuItem side = item("&Side Window", "workbench.action.toggleSidebarVisibility");
    side.checked = [this] { return panelOpen_; };
    menus.push_back({"&View",
                     {item("&Files", "workbench.view.explorer"), item("&Issues", "workbench.actions.view.problems"),
                      item("&Debugger", "workbench.view.debug"), side, line, item("&Bigger Text", "workbench.action.zoomIn"),
                      item("S&maller Text", "workbench.action.zoomOut"), wrap}});
    menus.push_back({"&Options", optionsItems()});
    menus.push_back({"&Help", {item("&Keys", "glasswork.showKeys"), item("&Commands...", "workbench.action.showCommands"),
                               item("&About", "glasswork.about")}});
    return menus;
}

std::vector<MenuItem> MainWindow::optionsItems() {
    const EditorSettings& settings = Services::shared().settings();
    std::vector<MenuItem> items;
    const char* const hotkeys[] = {"&Turbo", "&Low Tide", "&Phosphor"};
    const auto& schemes = bitmap::schemes();
    for (size_t index = 0; index < schemes.size(); ++index) {
        MenuItem scheme;
        scheme.title = index < 3 ? hotkeys[index] : schemes[index].name;
        scheme.command = "glasswork.colors." + schemes[index].name;
        std::string name = schemes[index].name;
        scheme.checked = [name] { return Services::shared().scheme().name == name; };
        items.push_back(scheme);
    }
    items.push_back(MenuItem::separator());
    MenuItem pixel;
    pixel.title = "Pi&xel Font";
    pixel.command = "glasswork.usePixelFont";
    pixel.checked = [] { return Services::shared().settings().bitmapFont.empty(); };
    items.push_back(pixel);
    MenuItem other;
    other.title = settings.bitmapFont.empty() ? "Other &Font..." : "&Font: " + settings.bitmapFont + "...";
    other.command = "glasswork.chooseFont";
    other.checked = [] { return !Services::shared().settings().bitmapFont.empty(); };
    items.push_back(other);
    items.push_back(MenuItem::separator());
    MenuItem smear;
    smear.title = "Cursor S&mear";
    smear.command = "glasswork.toggleCursorSmear";
    smear.checked = [] { return Services::shared().settings().vimCursorSmear; };
    items.push_back(smear);
    items.push_back(MenuItem::separator());
    MenuItem entry;
    entry.title = "&Settings...";
    entry.command = "workbench.action.openSettings";
    items.push_back(entry);
    entry.title = "&Keyboard...";
    entry.command = "glasswork.openKeybindings";
    items.push_back(entry);
    entry.title = "E&xtensions...";
    entry.command = "workbench.extensions.action.showInstalledExtensions";
    items.push_back(entry);
    entry.title = "&Commands...";
    entry.command = "workbench.action.showCommands";
    items.push_back(entry);
    return items;
}

MenuOverlay::Host MainWindow::menuHost() {
    MenuOverlay::Host host;
    host.titleRects = [this] { return menuTitleRects(); };
    host.showing = [this](int index) {
        openMenu_ = index;
        dirty_ = true;
    };
    host.run = [this](const MenuItem& item) {
        if (item.run) {
            item.run();
        } else {
            runCommand(item.command, item.arguments);
        }
        dirty_ = true;
    };
    host.enabled = [this](const MenuItem& item) {
        if (item.enabled) {
            return item.enabled();
        }
        auto found = commands_.find(item.command);
        if (found != commands_.end() && found->second.enabled) {
            return found->second.enabled();
        }
        return true;
    };
    host.shortcut = [this](const MenuItem& item) { return item.shortcut.empty() ? shortcutForCommand(item.command) : item.shortcut; };
    return host;
}

void MainWindow::openMenu(int index) {
    for (auto& overlay : overlays_) {
        if (dynamic_cast<MenuOverlay*>(overlay.get())) {
            overlay->close();
        }
    }
    dropClosedOverlays();
    std::vector<Menu> menus = buildMenus();
    if (index < 0 || index >= static_cast<int>(menus.size())) {
        return;
    }
    openMenu_ = index;
    pushOverlay(std::make_unique<MenuOverlay>(std::move(menus), index, menuHost()));
}

// ---------------------------------------------------------------------------
// Palettes and dialogs

void MainWindow::showCommandPalette() {
    std::vector<std::string> order = commandOrder_;
    pushOverlay(std::make_unique<PaletteOverlay>("Type a command...", [this, order](const std::string& query) {
        struct Ranked {
            int score;
            PaletteItem item;
        };
        std::vector<Ranked> ranked;
        for (const std::string& id : order) {
            const Command& command = commands_[id];
            if (command.hidden || (command.enabled && !command.enabled())) {
                continue;
            }
            PaletteItem item;
            item.title = command.title;
            item.shortcut = shortcutForCommand(id);
            int score = 0;
            if (!paletteMatch(query, item, &score)) {
                continue;
            }
            std::string commandId = id;
            item.run = [this, commandId] { runCommand(commandId); };
            ranked.push_back({score, std::move(item)});
        }
        if (extensionHost_) {
            for (PaletteItem& item : extensionHost_->commandItems(query)) {
                int score = 0;
                paletteMatch(query, item, &score);
                ranked.push_back({score, std::move(item)});
            }
        }
        if (!query.empty()) {
            std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
        }
        std::vector<PaletteItem> items;
        for (Ranked& entry : ranked) {
            items.push_back(std::move(entry.item));
        }
        return items;
    }));
}

void MainWindow::showKeys() {
    auto key = [this](const char* command, const char* fallback) {
        std::vector<KeyChord> chords = Services::shared().keybindings().shortcutFor(command, keyContext());
        std::string text = chords.empty() ? fallback : compactShortcutText(chords);
        text.resize(std::max<size_t>(text.size(), 7), ' ');
        return text;
    };
    DialogOverlay::Options options;
    options.title = "Keys";
    options.monospaced = true;
    options.lines = {
        "i a o    insert         " + key("workbench.action.debug.start", "F5") + " run, continue",
        "esc      normal         " + key("workbench.action.debug.stop", "Sh-F5") + " stop",
        "v V      visual         " + key("editor.debug.action.toggleBreakpoint", "F9") + " breakpoint",
        "hjkl w b move           " + key("workbench.action.debug.stepOver", "F10") + " step over",
        "x dd     delete         " + key("workbench.action.debug.stepInto", "F11") + " step into",
        "yy p     copy, paste    " + key("workbench.action.debug.stepOut", "Sh-F11") + " step out",
        "u ^R     undo, redo",
        "/ n N    search         " + key("workbench.action.quickOpen", "^P") + " open a file",
        ":w :q    save, quit     " + key("workbench.action.showCommands", "Sh-^P") + " commands",
        "alt      menu bar       " + key("workbench.action.terminal.toggleTerminal", "^`") + " terminal",
    };
    pushOverlay(std::make_unique<DialogOverlay>(options, [](int, const std::string&) {}));
}

void MainWindow::showAbout() {
    DialogOverlay::Options options;
    options.title = "About";
    options.lines = {"Glasswork Bitmap " GLASSWORK_VERSION, "", "A code editor drawn one pixel at a time, with Vim, a terminal, a debugger, "
                                                               "language servers and VS Code extensions.",
                     "", "Settings and extensions live in " + abbreviateHome(utf8FromPath(Services::shared().supportFolder()))};
    pushOverlay(std::make_unique<DialogOverlay>(options, [](int, const std::string&) {}));
}

void MainWindow::chooseFont() {
    setStatusMessage("Looking for fonts...");
    std::weak_ptr<bool> alive = alive_;
    runInBackground([this, alive] {
        auto families = std::make_shared<std::vector<std::string>>(ui::monospaceFamilies());
        runOnMain([this, alive, families] {
            if (alive.expired()) {
                return;
            }
            temporaryMessage_.clear();
            if (families->empty()) {
                showMessage("Fonts", "No monospaced fonts were found.");
                return;
            }
            std::string current = Services::shared().settings().bitmapFont;
            pushOverlay(std::make_unique<PaletteOverlay>("Font for the code...", [families, current](const std::string& query) {
                std::vector<PaletteItem> items;
                for (const std::string& family : *families) {
                    PaletteItem item;
                    item.title = family;
                    item.subtitle = family == current ? "in use" : "";
                    if (!paletteMatch(query, item, nullptr)) {
                        continue;
                    }
                    item.run = [family] {
                        Services::shared().updateSettings([family](EditorSettings& settings) { settings.bitmapFont = family; });
                    };
                    items.push_back(std::move(item));
                }
                return items;
            }));
        });
    });
}

void MainWindow::zoom(int steps) {
    Services::shared().updateSettings([steps](EditorSettings& settings) {
        settings.fontSize = std::clamp(settings.fontSize + steps, 6.0, 48.0);
    });
    setStatusMessage("Text size " + std::to_string(static_cast<int>(Services::shared().settings().fontSize)), 2);
}

}  // namespace gw::app
