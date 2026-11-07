#include "Input.hpp"

#include <cctype>
#include <map>

namespace gw::app {

namespace {

std::string keyLabel(const std::string& key) {
    static const std::map<std::string, std::string> names = {
        {"enter", "Enter"},   {"escape", "Esc"},   {"tab", "Tab"},        {"space", "Space"}, {"backspace", "Backspace"},
        {"delete", "Del"},    {"up", "Up"},        {"down", "Down"},      {"left", "Left"},   {"right", "Right"},
        {"home", "Home"},     {"end", "End"},      {"pageup", "PgUp"},    {"pagedown", "PgDn"}, {"insert", "Ins"},
    };
    auto found = names.find(key);
    if (found != names.end()) {
        return found->second;
    }
    if (key.size() >= 2 && key[0] == 'f' && std::isdigit(static_cast<unsigned char>(key[1]))) {
        return "F" + key.substr(1);
    }
    std::string label = key;
    for (char& character : label) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return label;
}

}  // namespace

std::string shortcutText(const std::vector<KeyChord>& chords) {
    std::string text;
    for (const KeyChord& chord : chords) {
        if (!text.empty()) {
            text += " ";
        }
        if (chord.control) {
            text += "Ctrl+";
        }
        if (chord.shift) {
            text += "Shift+";
        }
        if (chord.option) {
            text += "Alt+";
        }
        if (chord.command) {
            text += "Win+";
        }
        text += keyLabel(chord.key);
    }
    return text;
}

std::string compactShortcutText(const std::vector<KeyChord>& chords) {
    std::string text;
    for (const KeyChord& chord : chords) {
        if (!text.empty()) {
            text += " ";
        }
        if (chord.option) {
            text += "Alt-";
        }
        if (chord.shift) {
            text += "Sh-";
        }
        if (chord.control) {
            text += "^";
        }
        text += keyLabel(chord.key);
    }
    return text;
}

const nlohmann::json& defaultKeybindings() {
    static const nlohmann::json table = nlohmann::json::parse(R"json([
      { "key": "ctrl+n", "command": "workbench.action.files.newUntitledFile" },
      { "key": "ctrl+shift+n", "command": "workbench.action.newWindow" },
      { "key": "ctrl+o", "command": "workbench.action.files.openFile" },
      { "key": "ctrl+k ctrl+o", "command": "workbench.action.files.openFolder" },
      { "key": "ctrl+alt+shift+o", "command": "glasswork.openFolderInNewWindow" },
      { "key": "ctrl+s", "command": "workbench.action.files.save" },
      { "key": "ctrl+shift+s", "command": "workbench.action.files.saveAs" },
      { "key": "ctrl+alt+s", "command": "workbench.action.files.saveAll" },
      { "key": "ctrl+f4", "command": "workbench.action.closeActiveEditor" },
      { "key": "ctrl+w", "command": "workbench.action.closeActiveEditor" },
      { "key": "ctrl+alt+t", "command": "workbench.action.closeOtherEditors" },
      { "key": "ctrl+shift+t", "command": "workbench.action.reopenClosedEditor" },
      { "key": "ctrl+shift+w", "command": "workbench.action.closeWindow" },
      { "key": "ctrl+q", "command": "workbench.action.quit" },

      { "key": "ctrl+z", "command": "undo", "when": "!terminalFocus" },
      { "key": "ctrl+shift+z", "command": "redo", "when": "!terminalFocus" },
      { "key": "ctrl+y", "command": "redo", "when": "!terminalFocus" },
      { "key": "shift+delete", "command": "editor.action.clipboardCutAction", "when": "editorTextFocus" },
      { "key": "ctrl+insert", "command": "editor.action.clipboardCopyAction", "when": "editorTextFocus" },
      { "key": "shift+insert", "command": "editor.action.clipboardPasteAction", "when": "editorTextFocus" },
      { "key": "ctrl+x", "command": "editor.action.clipboardCutAction", "when": "!terminalFocus" },
      { "key": "ctrl+c", "command": "editor.action.clipboardCopyAction", "when": "!terminalFocus" },
      { "key": "ctrl+v", "command": "editor.action.clipboardPasteAction", "when": "!terminalFocus" },
      { "key": "ctrl+a", "command": "editor.action.selectAll", "when": "!terminalFocus" },
      { "key": "ctrl+/", "command": "editor.action.commentLine", "when": "editorTextFocus" },
      { "key": "ctrl+]", "command": "editor.action.indentLines", "when": "editorTextFocus" },
      { "key": "alt+up", "command": "editor.action.moveLinesUpAction", "when": "editorTextFocus" },
      { "key": "alt+down", "command": "editor.action.moveLinesDownAction", "when": "editorTextFocus" },
      { "key": "shift+alt+up", "command": "editor.action.copyLinesUpAction", "when": "editorTextFocus" },
      { "key": "shift+alt+down", "command": "editor.action.copyLinesDownAction", "when": "editorTextFocus" },
      { "key": "ctrl+shift+k", "command": "editor.action.deleteLines", "when": "editorTextFocus" },
      { "key": "ctrl+j", "command": "editor.action.joinLines", "when": "editorTextFocus" },
      { "key": "ctrl+enter", "command": "editor.action.insertLineAfter", "when": "editorTextFocus" },
      { "key": "ctrl+shift+enter", "command": "editor.action.insertLineBefore", "when": "editorTextFocus" },
      { "key": "shift+alt+right", "command": "editor.action.smartSelect.expand", "when": "editorTextFocus" },
      { "key": "shift+alt+left", "command": "editor.action.smartSelect.shrink", "when": "editorTextFocus" },
      { "key": "ctrl+shift+\\", "command": "editor.action.jumpToBracket", "when": "editorTextFocus" },
      { "key": "ctrl+d", "command": "editor.action.addSelectionToNextFindMatch", "when": "editorTextFocus" },
      { "key": "ctrl+shift+l", "command": "editor.action.selectHighlights", "when": "editorTextFocus" },
      { "key": "ctrl+alt+up", "command": "editor.action.insertCursorAbove", "when": "editorTextFocus" },
      { "key": "ctrl+alt+down", "command": "editor.action.insertCursorBelow", "when": "editorTextFocus" },
      { "key": "ctrl+shift+[", "command": "editor.fold", "when": "editorTextFocus" },
      { "key": "ctrl+shift+]", "command": "editor.unfold", "when": "editorTextFocus" },
      { "key": "ctrl+k ctrl+0", "command": "editor.foldAll", "when": "editorTextFocus" },
      { "key": "ctrl+k ctrl+j", "command": "editor.unfoldAll", "when": "editorTextFocus" },
      { "key": "alt+z", "command": "editor.action.toggleWordWrap" },

      { "key": "ctrl+f", "command": "actions.find", "when": "!terminalFocus" },
      { "key": "ctrl+h", "command": "editor.action.startFindReplace", "when": "!terminalFocus" },
      { "key": "f3", "command": "editor.action.nextMatchFindAction" },
      { "key": "shift+f3", "command": "editor.action.previousMatchFindAction" },
      { "key": "ctrl+e", "command": "glasswork.useSelectionForFind", "when": "editorTextFocus" },

      { "key": "ctrl+b", "command": "workbench.action.toggleSidebarVisibility", "when": "!terminalFocus" },
      { "key": "ctrl+shift+e", "command": "workbench.view.explorer" },
      { "key": "ctrl+shift+d", "command": "workbench.view.debug" },
      { "key": "ctrl+shift+m", "command": "workbench.actions.view.problems" },
      { "key": "ctrl+pagedown", "command": "workbench.action.nextEditor" },
      { "key": "ctrl+tab", "command": "workbench.action.nextEditor" },
      { "key": "ctrl+pageup", "command": "workbench.action.previousEditor" },
      { "key": "ctrl+shift+tab", "command": "workbench.action.previousEditor" },
      { "key": "ctrl+shift+v", "command": "markdown.showPreview", "when": "!terminalFocus" },
      { "key": "ctrl+shift+=", "command": "workbench.action.zoomIn" },
      { "key": "ctrl+=", "command": "workbench.action.zoomIn" },
      { "key": "ctrl+-", "command": "workbench.action.zoomOut" },
      { "key": "ctrl+0", "command": "workbench.action.zoomReset" },

      { "key": "ctrl+p", "command": "workbench.action.quickOpen", "when": "!terminalFocus" },
      { "key": "f1", "command": "workbench.action.showCommands" },
      { "key": "ctrl+shift+p", "command": "workbench.action.showCommands" },
      { "key": "alt+left", "command": "workbench.action.navigateBack", "when": "!terminalFocus" },
      { "key": "alt+right", "command": "workbench.action.navigateForward", "when": "!terminalFocus" },
      { "key": "ctrl+g", "command": "workbench.action.gotoLine", "when": "!terminalFocus" },
      { "key": "ctrl+6", "command": "glasswork.jumpToSection", "when": "!terminalFocus" },
      { "key": "alt+f5", "command": "workbench.action.editor.nextChange", "when": "editorTextFocus" },
      { "key": "shift+alt+f5", "command": "workbench.action.editor.previousChange", "when": "editorTextFocus" },
      { "key": "ctrl+space", "command": "editor.action.triggerSuggest", "when": "editorTextFocus" },
      { "key": "f12", "command": "editor.action.revealDefinition" },
      { "key": "shift+f12", "command": "editor.action.goToReferences" },
      { "key": "ctrl+shift+o", "command": "workbench.action.gotoSymbol", "when": "!terminalFocus" },
      { "key": "ctrl+t", "command": "workbench.action.showAllSymbols", "when": "!terminalFocus" },
      { "key": "ctrl+.", "command": "editor.action.quickFix", "when": "editorTextFocus" },
      { "key": "f2", "command": "editor.action.rename" },
      { "key": "shift+alt+f", "command": "editor.action.formatDocument", "when": "editorTextFocus" },

      { "key": "ctrl+shift+b", "command": "workbench.action.tasks.build" },
      { "key": "ctrl+alt+b", "command": "workbench.action.tasks.runTask" },
      { "key": "ctrl+f5", "command": "glasswork.runCurrentFile" },
      { "key": "f5", "command": "workbench.action.debug.start" },
      { "key": "ctrl+alt+f5", "command": "glasswork.debugCurrentFile" },
      { "key": "shift+f5", "command": "workbench.action.debug.stop" },
      { "key": "f10", "command": "workbench.action.debug.stepOver" },
      { "key": "f11", "command": "workbench.action.debug.stepInto" },
      { "key": "shift+f11", "command": "workbench.action.debug.stepOut" },
      { "key": "f9", "command": "editor.debug.action.toggleBreakpoint" },

      { "key": "ctrl+`", "command": "workbench.action.terminal.toggleTerminal" },
      { "key": "ctrl+shift+`", "command": "workbench.action.terminal.new" },

      { "key": "ctrl+,", "command": "workbench.action.openSettings" },
      { "key": "ctrl+k ctrl+s", "command": "glasswork.openKeybindings" },
      { "key": "ctrl+shift+x", "command": "workbench.extensions.search" }
    ])json");
    return table;
}

}  // namespace gw::app
