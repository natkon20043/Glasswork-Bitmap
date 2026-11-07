#pragma once

#include "Canvas.hpp"
#include "Font.hpp"
#include "Input.hpp"

#include <functional>
#include <string>

namespace gw::app {

/// One line of editable text in pixel letters: the palette's query, the find bar's fields, a
/// dialog's answer. Handles the caret, selection, words, the clipboard and undo of one step.
class TextField {
public:
    // ---- Text
    const std::string& text() const { return text_; }
    void setText(const std::string& text, bool selectAll = false);
    void selectAll();
    bool empty() const { return text_.empty(); }

    // ---- Input (true when used)
    bool keyDown(const KeyPress& key);
    void insert(const std::string& text);
    /// A click at `x` (relative to the field's text start) moves the caret there.
    void clickAt(int x, int cellWidth, bool extend);

    // ---- Drawing
    /// Text from (x, y) clipped to `width`; the caret blinks only when `focused`.
    void draw(ui::Canvas& canvas, ui::Font& font, int x, int y, int width, ui::Color text, ui::Color selection,
              ui::Color selectionText, ui::Color caret, bool focused, bool shadow, const std::string& placeholder = "",
              ui::Color placeholderColor = 0);

    // ---- Callbacks
    std::function<void()> onChange;

private:
    size_t previousBoundary(size_t index) const;
    size_t nextBoundary(size_t index) const;
    size_t previousWord(size_t index) const;
    size_t nextWord(size_t index) const;
    void replaceSelection(const std::string& text);
    void changed();

    std::string text_;
    size_t caret_ = 0;
    size_t anchor_ = 0;
    std::string undoText_;
    size_t undoCaret_ = 0;
    int scrollCells_ = 0;
};

}  // namespace gw::app
