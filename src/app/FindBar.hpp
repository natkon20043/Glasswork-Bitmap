#pragma once

#include "Canvas.hpp"
#include "Document.hpp"
#include "Input.hpp"
#include "ProjectSearch.hpp"
#include "TextField.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gw::app {

class EditorView;

/// Find and replace, a grey box floating over the top right of the code. Searches as you type,
/// marks every match (the current one stronger), with case, whole word and regex options.
class FindBar {
public:
    explicit FindBar(EditorView& editor) : editor_(editor) {}

    // ---- Showing
    void show(const std::string& text, bool replace);
    void close();
    bool visible() const { return visible_; }

    // ---- Searching
    void findNext() { step(1); }
    void findPrevious() { step(-1); }
    /// Sets the search without showing the bar (Ctrl+E).
    void setSearchText(const std::string& text);
    /// The document was edited or switched: recompute the matches.
    void documentChanged();

    // ---- Layout, drawing and events
    /// Placed at the top right of `editorArea`.
    void layout(const ui::Rect& editorArea);
    const ui::Rect& frame() const { return box_; }
    void draw(ui::Canvas& canvas, bool focused);
    bool mouseDown(const MouseEvent& event);
    bool keyDown(const KeyPress& key);
    void textInput(const std::string& text);

    // ---- Callbacks
    std::function<void()> onClose;
    std::function<void()> onNeedsDisplay;
    /// The bar wants the keyboard (a click on it).
    std::function<void()> onFocus;

private:
    search::Query query() const;
    void recompute(bool select);
    void step(int direction);
    void replaceOne();
    void replaceAll();
    std::string replacementFor(const Range& match, Document& document);
    void changed();

    EditorView& editor_;
    bool visible_ = false;
    bool replaceVisible_ = false;
    bool replaceFocused_ = false;
    bool caseSensitive_ = false;
    bool wholeWord_ = false;
    bool regex_ = false;
    TextField find_;
    TextField replace_;
    std::unique_ptr<search::Matcher> matcher_;
    std::vector<Range> matches_;
    long current_ = -1;
    Position anchor_;
    std::string count_;
    bool countIsProblem_ = false;
    ui::Rect box_;
    struct Button {
        ui::Rect rect;
        int action;
    };
    std::vector<Button> buttons_;
    ui::Rect findField_;
    ui::Rect replaceField_;
};

}  // namespace gw::app
