#pragma once

#include "Canvas.hpp"
#include "Font.hpp"
#include "Input.hpp"
#include "Pty.hpp"
#include "Terminal.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gw::app {

/// A shell (or another program) in a pseudo-terminal plus the screen showing its output. Output
/// arrives on a background thread and reaches the screen on the main thread in slices, so heavy
/// output never freezes the window.
class TerminalSession {
public:
    TerminalSession(const std::string& folder, const std::string& command, int columns, int rows);
    ~TerminalSession();

    void send(const std::string& bytes);
    void paste(const std::string& text);
    void resize(int columns, int rows);
    void terminate();

    term::Screen& screen() { return *screen_; }
    bool isRunning() const;
    std::string title() const;
    std::string startError() const { return error_; }
    /// The shell is waiting for a command (nothing running in front of it).
    bool isIdleAtPrompt() const;
    const std::string& folder() const { return folder_; }

    std::function<void()> onChange;
    std::function<void(int code)> onExit;

private:
    void received(std::string_view bytes);
    void feedSlice();

    std::unique_ptr<term::Screen> screen_;
    std::unique_ptr<Pty> pty_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::mutex pendingMutex_;
    std::string pending_;
    bool feeding_ = false;
    std::string shellName_;
    std::string folder_;
    std::string error_;
    bool exited_ = false;
};

/// The terminal under the code: tabs for several shells, the one in view, and buttons for a new
/// one, closing one and hiding the panel. Drawn in pixel text in the scheme's colors.
class TerminalPanel {
public:
    TerminalPanel();
    ~TerminalPanel();

    // ---- Terminals
    void newTerminal(const std::string& command = "");
    /// Uses the terminal set aside for tasks if its shell is free, otherwise opens a new one.
    void runTaskCommand(const std::string& command);
    void closeCurrentTerminal();
    void clear();
    size_t count() const { return sessions_.size(); }
    void sendText(const std::string& text);

    // ---- Layout and drawing
    void setFrame(const ui::Rect& frame);
    const ui::Rect& frame() const { return frame_; }
    void draw(ui::Canvas& canvas, bool focused);
    void reloadAppearance();

    // ---- Events
    bool mouseDown(const MouseEvent& event);
    void mouseDragged(const MouseEvent& event);
    void mouseUp(const MouseEvent& event);
    bool wheel(const WheelEvent& event);
    bool keyDown(const KeyPress& key);
    void textInput(const std::string& text);
    void focusChanged(bool focused);
    bool hasSelection() const { return hasSelection_; }
    void copySelection();
    void paste();
    void selectAll();

    // ---- Properties
    std::string projectFolder;
    std::string shell;

    // ---- Callbacks
    std::function<void()> onNeedsDisplay;
    /// Ctrl-click on "src/main.cpp:42:7" (line and column 1-based, 0 when absent).
    std::function<void(const std::string& path, int line, int column)> onOpenPath;
    std::function<void()> onHide;
    /// The tab strip's "+": a new terminal.
    std::function<void()> onFocus;
    /// Right-click: a menu at (x, y).
    std::function<void(int x, int y)> onContextMenu;

private:
    struct Point {
        long row = 0;
        int column = 0;
    };
    TerminalSession* current() const;
    void select(size_t index);
    ui::Rect stripRect() const;
    ui::Rect screenRect() const;
    void updateGridSize();
    Point pointAt(int x, int y) const;
    bool selectionContains(long row, int column) const;
    std::string selectedText() const;
    bool reportMouse(const MouseEvent& event, int button, bool pressed, bool motion);
    void wordRangeAt(Point point, int& start, int& end) const;
    std::string tokenAt(Point point) const;
    void openLinkAt(Point point);
    std::string sequenceForKey(const KeyPress& key) const;
    Color colorFor(term::TermColor color, bool foreground, bool bold) const;

    ui::Rect frame_;
    std::vector<std::unique_ptr<TerminalSession>> sessions_;
    size_t selected_ = 0;
    long taskSession_ = -1;
    struct StripButton {
        ui::Rect rect;
        int action;  // >= 0: a tab; -1 new, -2 close, -3 hide
    };
    std::vector<StripButton> stripButtons_;
    std::shared_ptr<ui::Font> font_;
    Color palette_[256] = {};
    Color foreground_ = 0;
    Color background_ = 0;
    Color cursorColor_ = 0;
    Color selectionColor_ = 0;
    bool focused_ = false;
    long scrollOffset_ = 0;
    double scrollRemainder_ = 0;
    bool selecting_ = false;
    bool hasSelection_ = false;
    int granularity_ = 1;
    Point anchor_;
    Point head_;
    bool reportingMouse_ = false;
    int gridColumns_ = 0;
    int gridRows_ = 0;
};

}  // namespace gw::app
