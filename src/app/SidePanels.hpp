#pragma once

#include "Canvas.hpp"
#include "Debugger.hpp"
#include "Input.hpp"
#include "PixelArt.hpp"
#include "TextField.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gw::app {

/// One line of a ListView.
struct ListRow {
    std::string text;
    Color textColor = 0;
    bool hasIcon = false;
    ui::Icon icon = ui::Icon::File;
    Color iconColor = 0;
    bool highlighted = false;
    /// What choosing the row means: a path, "..", "+new", a "line:column".
    std::string payload;
};

/// Rows of pixel text with little icons, one line each: the side window's files and issues.
class ListView {
public:
    void setRows(std::vector<ListRow> rows);
    const std::vector<ListRow>& rows() const { return rows_; }
    void setFrame(const ui::Rect& frame) { frame_ = frame; }
    const ui::Rect& frame() const { return frame_; }

    void draw(ui::Canvas& canvas, bool focused);
    bool mouseDown(const MouseEvent& event);
    bool wheel(const WheelEvent& event);
    bool keyDown(const KeyPress& key);
    /// The row at a point, or -1.
    int rowAt(int x, int y) const;

    std::function<void(const ListRow& row)> onChoose;
    /// Right-click on a row (or -1 for the empty space below them).
    std::function<void(int row, int x, int y)> onContextMenu;

private:
    void ensureVisible(size_t index);
    int visibleRows() const;

    std::vector<ListRow> rows_;
    ui::Rect frame_;
    size_t scroll_ = 0;
    long cursor_ = -1;
};

/// "Run and Debug" in the side window: the session's state, buttons, the call stack, the selected
/// frame's variables, and a console for the program's output and for evaluating expressions.
class DebugPanel {
public:
    explicit DebugPanel(Debugger& debugger);

    void reload();
    void appendConsole(const std::string& text, const std::string& category);
    void clearConsole();

    void setFrame(const ui::Rect& frame) { frame_ = frame; }
    void draw(ui::Canvas& canvas, bool focused);
    bool mouseDown(const MouseEvent& event);
    bool wheel(const WheelEvent& event);
    bool keyDown(const KeyPress& key);
    void textInput(const std::string& text);
    /// The console's field has the keyboard.
    bool wantsKeys() const { return inputFocused_; }
    void blur() { inputFocused_ = false; }

    std::function<void()> onStart;
    std::function<void()> onNeedsDisplay;
    std::function<void()> onFocus;

private:
    struct VariableRow {
        std::shared_ptr<DebugVariable> variable;
        int depth = 0;
    };
    struct ConsoleLine {
        std::string text;
        Color color = 0;
    };
    std::vector<VariableRow> visibleVariables() const;
    void changed();

    Debugger& debugger_;
    ui::Rect frame_;
    std::vector<std::shared_ptr<DebugVariable>> scopes_;
    std::vector<ConsoleLine> console_;
    std::string partialLine_;
    std::string partialCategory_;
    TextField input_;
    bool inputFocused_ = false;
    size_t stackScroll_ = 0;
    size_t variableScroll_ = 0;
    size_t consoleScroll_ = 0;  // lines up from the bottom
    struct Hit {
        ui::Rect rect;
        int kind;  // 1 button, 2 frame, 3 variable, 4 input, 5 start
        int index;
    };
    std::vector<Hit> hits_;
    ui::Rect stackArea_;
    ui::Rect variableArea_;
    ui::Rect consoleArea_;
};

}  // namespace gw::app
