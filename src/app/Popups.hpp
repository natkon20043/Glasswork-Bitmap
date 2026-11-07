#pragma once

#include "Canvas.hpp"
#include "CompletionRank.hpp"
#include "Input.hpp"
#include "LanguageServer.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gw::app {

/// Lines of pixel text in a grey DOS box, wrapped to a width: hover text, documentation, a hunk.
class RetroTextBox {
public:
    void clear();
    void addLine(const std::string& text, Color color, int indent = 0);
    /// Markdown reduced to plain lines (fences, emphasis marks and heading marks go). Returns whether
    /// the first line is code, which then goes on the title strip.
    bool addMarkdown(const std::string& markdown, Color color);
    /// How many of the first lines sit on the title strip.
    void setBandLines(size_t lines) { bandLines_ = lines; }
    /// Wraps at `columns` and returns the box's size (shadow included).
    ui::Rect layout(int columns, int maxLines);
    /// Body top left at (x, y).
    void draw(ui::Canvas& canvas, int x, int y);
    bool empty() const { return lines_.empty(); }

private:
    struct TextLine {
        std::string text;
        Color color;
        int indent;
    };
    std::vector<TextLine> lines_;
    std::vector<TextLine> laidOut_;
    size_t bandLines_ = 0;
    int width_ = 0;
    int height_ = 0;
};

/// One suggestion from a language server (or a word from the file).
struct Suggestion {
    std::string label;
    std::string detail;
    std::string filterText;
    std::string sortText;
    int kind = 0;
    bool deprecated = false;
    bool preselect = false;
    std::string documentation;
    std::string commitCharacters;
    std::shared_ptr<lsp::Json> raw;
    bool resolved = false;
};

/// The completion list under the caret, ranked like VS Code's, with the chosen item's documentation
/// beside it. Never takes the keyboard: the editor forwards the keys it uses.
class CompletionPopup {
public:
    void setItems(std::vector<std::shared_ptr<Suggestion>> items);
    /// `caret` is the caret's cell in window coordinates; `window` is where the box may go.
    void show(const ui::Rect& caret, const ui::Rect& window);
    void filter(const std::string& prefix);
    void hide();
    bool visible() const { return visible_; }
    size_t itemCount() const { return all_.size(); }
    std::shared_ptr<Suggestion> selected() const;
    /// "up", "down", "pageup", "pagedown", "enter", "tab", "escape": true if used.
    bool handleKey(const std::string& action);
    void recordAccepted(const Suggestion& item);
    void documentationChanged() {}

    void draw(ui::Canvas& canvas);
    bool mouseDown(const MouseEvent& event);
    bool wheel(const WheelEvent& event);
    bool contains(int x, int y) const;

    std::function<void(std::shared_ptr<Suggestion>)> onAccept;
    std::function<void(std::shared_ptr<Suggestion>)> onSelectionChanged;

private:
    ui::Rect listRect() const;
    void selectionChanged();

    std::vector<std::shared_ptr<Suggestion>> all_;
    std::vector<std::shared_ptr<Suggestion>> shown_;
    std::vector<CompletionCandidate> candidates_;
    CompletionHistory history_;
    size_t selected_ = 0;
    size_t scroll_ = 0;
    bool visible_ = false;
    ui::Rect caret_;
    ui::Rect window_;
};

/// A box near a word: problems on that line, then the server's hover text; or signature help.
class HoverBox {
public:
    void show(const std::string& markdown, const std::vector<std::string>& problems, const ui::Rect& word, const ui::Rect& window);
    /// The signature with the current parameter in the hotkey color, above the caret.
    void showSignature(const std::string& signature, int activeStart, int activeEnd, const std::string& documentation,
                       const ui::Rect& caret, const ui::Rect& window);
    void hide() { visible_ = false; }
    bool visible() const { return visible_; }
    void draw(ui::Canvas& canvas);
    bool contains(int x, int y) const { return visible_ && rect_.contains(x, y); }

private:
    RetroTextBox box_;
    ui::Rect rect_;
    bool visible_ = false;
    bool signature_ = false;
    std::string signatureText_;
    int activeStart_ = -1;
    int activeEnd_ = -1;
};

/// What a gutter change bar shows: the old and new lines, and buttons for the neighbouring changes,
/// staging and reverting.
class ChangeBox {
public:
    void show(const std::string& title, const std::vector<std::string>& oldLines, const std::vector<std::string>& newLines,
              const ui::Rect& bar, const ui::Rect& window);
    void hide() { visible_ = false; }
    bool visible() const { return visible_; }
    void draw(ui::Canvas& canvas);
    /// True when the click was on the box.
    bool mouseDown(const MouseEvent& event);

    std::function<void()> onStage;
    std::function<void()> onRevert;
    std::function<void()> onPrevious;
    std::function<void()> onNext;

private:
    RetroTextBox box_;
    ui::Rect rect_;
    bool visible_ = false;
    std::string title_;
    struct Button {
        ui::Rect rect;
        int action;
    };
    std::vector<Button> buttons_;
};

}  // namespace gw::app
