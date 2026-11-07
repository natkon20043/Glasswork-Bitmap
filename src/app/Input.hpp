#pragma once

#include "Keybindings.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace gw::app {

/// A key going down, as the windows see it.
struct KeyPress {
    /// Lower case, VS Code's names: "a", "5", "f12", "enter", "escape", "tab", "space", "backspace",
    /// "delete", "up", "down", "left", "right", "home", "end", "pageup", "pagedown", "insert", "/", "[".
    /// Empty for keys with no name (modifiers alone).
    std::string key;
    bool control = false;
    bool shift = false;
    bool alt = false;
    /// The Windows or Super key.
    bool meta = false;
    bool repeat = false;
    /// The keypad's own Enter and the like, for programs that care.
    bool keypad = false;

    bool plain() const { return !control && !shift && !alt && !meta; }
    bool is(const char* name, bool withControl = false, bool withShift = false, bool withAlt = false) const {
        return key == name && control == withControl && shift == withShift && alt == withAlt && !meta;
    }
    KeyChord chord() const {
        KeyChord chord;
        chord.command = meta;
        chord.control = control;
        chord.option = alt;
        chord.shift = shift;
        chord.key = key;
        return chord;
    }
};

struct MouseEvent {
    int x = 0;
    int y = 0;
    /// 1 left, 2 middle, 3 right.
    int button = 1;
    int clicks = 1;
    bool control = false;
    bool shift = false;
    bool alt = false;
};

struct WheelEvent {
    int x = 0;
    int y = 0;
    /// In lines (positive: away from the user, so the view scrolls up).
    double dx = 0;
    double dy = 0;
    bool control = false;
    bool shift = false;
};

// ---- Shortcut text
/// "Ctrl+Shift+P" for menus and the command palette; a sequence is joined with spaces.
std::string shortcutText(const std::vector<KeyChord>& chords);
/// "^S" style, for the status line's keys.
std::string compactShortcutText(const std::vector<KeyChord>& chords);

/// A command anything can run: a menu, the command palette, a key, an extension.
struct Command {
    /// VS Code's id where it has one ("workbench.action.files.save"), else "glasswork.<name>".
    std::string id;
    /// As listed in the command palette ("File: Save").
    std::string title;
    std::function<void(const nlohmann::json& arguments)> run;
    /// Null: always available.
    std::function<bool()> enabled;
    /// Null: not a choice that is on or off.
    std::function<bool()> checked;
    /// Hidden from the command palette (things only menus or keys use).
    bool hidden = false;
};

/// The default shortcuts, in keybindings.json form (the table every window starts from; extensions
/// and the user's keybindings.json are layered over it).
const nlohmann::json& defaultKeybindings();

}  // namespace gw::app
