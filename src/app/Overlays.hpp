#pragma once

#include "Canvas.hpp"
#include "Input.hpp"
#include "TextField.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gw::app {

/// Something floating over the window that takes the keyboard while it is up: a menu, a dialog,
/// the palette.
class Overlay {
public:
    virtual ~Overlay() = default;

    /// Places itself in the window (device pixels).
    virtual void layout(const ui::Rect& window) { window_ = window; }
    virtual void draw(ui::Canvas& canvas) = 0;
    virtual void keyDown(const KeyPress& key) = 0;
    virtual void textInput(const std::string& text) {}
    /// False lets the click through (and closes the overlay when it should).
    virtual bool mouseDown(const MouseEvent& event) = 0;
    virtual void mouseDragged(const MouseEvent& event) {}
    virtual void mouseUp(const MouseEvent& event) {}
    virtual void mouseMoved(const MouseEvent& event) {}
    virtual void wheel(const WheelEvent& event) {}
    /// Typing goes to a text field (for the IME and Alt handling).
    virtual bool wantsText() const { return false; }

    bool closed() const { return closed_; }
    void close() { closed_ = true; }

protected:
    ui::Rect window_;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Menus

/// An entry in a menu. A "&" in the title marks its hotkey letter.
struct MenuItem {
    static MenuItem separator() {
        MenuItem item;
        item.isSeparator = true;
        return item;
    }

    std::string title;
    /// Run when chosen; the command id is used for the shortcut text and to check availability.
    std::string command;
    nlohmann::json arguments;
    std::function<void()> run;
    std::function<bool()> enabled;
    std::function<bool()> checked;
    /// Shown right-aligned; filled from the keybindings when empty.
    std::string shortcut;
    bool isSeparator = false;
};

struct Menu {
    /// "≡" draws the little menu icon.
    std::string title;
    std::vector<MenuItem> items;
};

/// A drop-down menu (the menu bar's, or a right-click menu at a point): grey box, a thin black frame
/// line, a green bar on the chosen item, red hotkey letters, shortcuts right-aligned. Arrows move,
/// Return or a hotkey runs, Escape closes; left and right go to the neighbouring menus.
class MenuOverlay : public Overlay {
public:
    struct Host {
        /// Where each menu bar title is (empty for a menu at a point).
        std::function<std::vector<ui::Rect>()> titleRects;
        /// The menu showing changed (for the highlighted title).
        std::function<void(int index)> showing;
        std::function<void(const MenuItem& item)> run;
        std::function<bool(const MenuItem& item)> enabled;
        std::function<std::string(const MenuItem& item)> shortcut;
    };

    /// The menu bar's menu `index`.
    MenuOverlay(std::vector<Menu> menus, int index, Host host);
    /// A menu on its own, its top left at (x, y).
    MenuOverlay(Menu menu, int x, int y, Host host);

    void layout(const ui::Rect& window) override;
    void draw(ui::Canvas& canvas) override;
    void keyDown(const KeyPress& key) override;
    void textInput(const std::string& text) override;
    bool mouseDown(const MouseEvent& event) override;
    void mouseMoved(const MouseEvent& event) override;
    void mouseUp(const MouseEvent& event) override;
    bool wantsText() const override { return true; }
    int menuIndex() const { return index_; }

private:
    void show(int index);
    void moveHighlight(int step);
    void choose(int row);
    ui::Rect dropdownRect() const;
    int rowAt(int x, int y) const;
    int titleAt(int x, int y) const;
    int itemHeight(const MenuItem& item) const;

    std::vector<Menu> menus_;
    int index_ = 0;
    Host host_;
    bool atPoint_ = false;
    int pointX_ = 0;
    int pointY_ = 0;
    int highlighted_ = -1;
    std::vector<bool> enabled_;
    std::vector<std::string> shortcuts_;
    bool releaseChooses_ = false;
};

// ---------------------------------------------------------------------------
// Dialogs

/// A grey box with a white title strip, lines of text, an optional field to type in, and buttons
/// (the first is the default: Return). Escape answers with the last button, or -1 without buttons.
class DialogOverlay : public Overlay {
public:
    struct Options {
        std::string title;
        std::vector<std::string> lines;
        /// "&Save": a hotkey letter runs the button.
        std::vector<std::string> buttons{"OK"};
        bool hasField = false;
        std::string fieldText;
        std::string placeholder;
        bool selectField = true;
        /// Lines keep their own spacing (a table of keys) instead of being wrapped.
        bool monospaced = false;
        /// -1: Escape picks the last button.
        int cancelButton = -1;
    };

    DialogOverlay(Options options, std::function<void(int button, const std::string& text)> done);

    void layout(const ui::Rect& window) override;
    void draw(ui::Canvas& canvas) override;
    void keyDown(const KeyPress& key) override;
    void textInput(const std::string& text) override;
    bool mouseDown(const MouseEvent& event) override;
    bool wantsText() const override { return true; }

private:
    void finish(int button);
    ui::Rect buttonRect(size_t index) const;

    Options options_;
    std::vector<std::string> buttonTitles_;
    std::vector<int> buttonHotkeys_;
    std::vector<std::string> wrapped_;
    std::function<void(int, const std::string&)> done_;
    TextField field_;
    ui::Rect box_;
    int focusedButton_ = 0;
};

// ---------------------------------------------------------------------------
// The palette

/// One palette row.
struct PaletteItem {
    std::string title;
    std::string subtitle;
    std::string shortcut;
    /// Byte offsets into `title` of the characters the query matched (drawn in the hotkey color).
    std::vector<uint32_t> matched;
    std::function<void()> run;
};

/// Quick Open, the command palette, Go to Line and every other pick-from-a-list: a box near the top
/// of the window with a field and up to ten rows. Type to filter, arrows to move, Return to run.
class PaletteOverlay : public Overlay {
public:
    using Provider = std::function<std::vector<PaletteItem>(const std::string& query)>;

    PaletteOverlay(std::string placeholder, Provider provider, std::function<void()> onDismiss = nullptr);

    void layout(const ui::Rect& window) override;
    void draw(ui::Canvas& canvas) override;
    void keyDown(const KeyPress& key) override;
    void textInput(const std::string& text) override;
    bool mouseDown(const MouseEvent& event) override;
    void mouseMoved(const MouseEvent& event) override;
    void wheel(const WheelEvent& event) override;
    bool wantsText() const override { return true; }

    /// Re-asks the provider (for lists that fill in later).
    void refresh();
    void setQuery(const std::string& query);
    const std::string& query() const { return field_.text(); }

private:
    void reload();
    void run(size_t index);
    void dismiss();
    ui::Rect rowRect(size_t visibleIndex) const;

    std::string placeholder_;
    Provider provider_;
    std::function<void()> onDismiss_;
    TextField field_;
    std::vector<PaletteItem> items_;
    size_t selected_ = 0;
    size_t scroll_ = 0;
    ui::Rect box_;
    bool ran_ = false;
};

/// Matches the query against the title for the palette: highlights and a score, or nothing.
bool paletteMatch(const std::string& query, PaletteItem& item, int* score);

}  // namespace gw::app
