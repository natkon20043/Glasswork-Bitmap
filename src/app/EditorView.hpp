#pragma once

#include "Canvas.hpp"
#include "CursorSmear.hpp"
#include "Document.hpp"
#include "Extensions.hpp"
#include "Folding.hpp"
#include "Font.hpp"
#include "Highlighter.hpp"
#include "Input.hpp"
#include "LanguageServer.hpp"
#include "LineDiff.hpp"
#include "RowMap.hpp"
#include "Sections.hpp"
#include "SemanticTokens.hpp"
#include "Theme.hpp"
#include "Vim.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gw::app {

/// A problem in a file, in the document's units (UTF-8 bytes).
struct Diagnostic {
    Range range;
    /// 1 error, 2 warning, 3 information, 4 hint.
    int severity = 1;
    std::string message;
    std::string source;
    /// The server's own form, sent back with code-action requests.
    std::shared_ptr<lsp::Json> raw;
};

/// An inline hint (parameter name before an argument, inferred type after a name).
struct InlayHint {
    Position position;
    std::string label;
    bool isType = false;
    bool padLeft = false;
    bool padRight = false;
};

/// An extension's look for a stretch of text (`setDecorations`).
struct Decoration {
    Range range;
    std::optional<Color> background;
    std::optional<Color> foreground;
    std::optional<Color> border;
    bool wholeLine = false;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    std::string afterText;
    std::optional<Color> afterColor;
};

/// The code editor: draws a document in the pixel font (or the font chosen in Options) with the
/// line numbers beside it, and handles typing, Vim, selection, multiple cursors and the editing
/// commands. Coordinates are device pixels; `frame` is where it sits in the window.
class EditorView {
public:
    EditorView();

    // ---- Document
    void setDocument(Document* document, Highlighter* highlighter, const LanguageConfiguration& configuration);
    Document* document() const { return document_; }
    /// Every change to any document comes here, so the shown one's caches stay right.
    void documentChanged(Document* document, const Change& change);
    /// Edits as one undo step, then redraws.
    void editDocument(const std::function<void(Document&)>& edits);
    void replaceRange(Range range, const std::string& text);

    // ---- Layout and looks
    void setFrame(const ui::Rect& frame);
    const ui::Rect& frame() const { return frame_; }
    /// Re-reads fonts and colors from the services.
    void reloadAppearance();
    void invalidateLayout();
    /// `usesTabs`: 1 tabs, 0 spaces, -1 decide from the file. `tabSize` 0 uses settings.json.
    void setIndentationOverride(int usesTabs, size_t tabSize);
    int lineHeight() const { return lineHeight_; }
    int charWidth() const { return charWidth_; }
    int gutterWidth() const;

    // ---- Scrolling (the window's frame draws the scrollbars)
    double scrollX() const { return scrollX_; }
    double scrollY() const { return scrollY_; }
    double contentWidth() const;
    double contentHeight() const;
    int visibleWidth() const;
    int visibleHeight() const { return frame_.h; }
    void scrollTo(double x, double y);

    // ---- Drawing
    void draw(ui::Canvas& canvas, bool focused);
    /// The smear or a blink needs another frame at this time (0: none).
    double nextFrameTime() const;
    /// Advances animations; true if something changed and it should be drawn again.
    bool tick(double now);

    // ---- Events
    bool mouseDown(const MouseEvent& event);
    void mouseDragged(const MouseEvent& event);
    void mouseUp(const MouseEvent& event);
    void mouseMoved(const MouseEvent& event);
    void mouseExited();
    bool wheel(const WheelEvent& event);
    bool keyDown(const KeyPress& key);
    void textInput(const std::string& text);
    void focusChanged(bool focused);

    // ---- Selection and navigation
    void selectionDidChange();
    void selectRange(Range range);
    void jumpToLine(size_t line);
    void jumpToPosition(Position position);
    void scrollCaretToVisible();
    void scrollToLineCentered(size_t line);
    void flashLines(size_t first, size_t count);
    size_t cursorCount() const;

    // ---- Queries
    const std::vector<Section>& sections() const { return sections_; }
    std::string wordAtCaret() const;
    std::string selectedText() const;
    /// Where a position is drawn, in window coordinates (one character wide, one line high).
    ui::Rect rectForPosition(Position position);
    std::vector<std::string> scopesAtCaret();
    /// "NORMAL", "INSERT", "VISUAL"... with a command in progress after it; empty when Vim is off.
    std::string vimStatus() const;
    bool insertsText() const;
    VimEngine& vim() { return vim_; }

    // ---- Marks and overlays
    void setLineChanges(std::vector<LineChange> changes);
    const std::vector<LineChange>& lineChanges() const { return lineChanges_; }
    void setLineTints(std::vector<Color> tints);
    void setInlayHints(std::vector<InlayHint> hints);
    void setDecorationGroups(const std::map<std::string, std::vector<Decoration>>& groups);
    void setDiagnostics(std::vector<Diagnostic> diagnostics);
    void setSemanticTokens(std::shared_ptr<const SemanticTokenSet> tokens);
    void setFindMatches(std::vector<Range> matches, long current);

    // ---- Debugging
    void toggleBreakpointAtLine(size_t line);
    void setExecutionLine(long line);

    // ---- Editing commands
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void selectAll();
    void toggleLineComment();
    void indentLines();
    void outdentLines();
    void moveLinesUp();
    void moveLinesDown();
    void duplicateLinesUp();
    void duplicateLinesDown();
    void deleteLines();
    void joinSelectedLines();
    void insertLineBelow();
    void insertLineAbove();
    void expandSelection();
    void shrinkSelection();
    void goToMatchingBracket();
    void makeUpperCase();
    void makeLowerCase();
    void makeTitleCase();
    void sortLinesAscending();
    void sortLinesDescending();
    void removeDuplicateLines();
    void addCursorAbove();
    void addCursorBelow();
    void selectAllOccurrences();
    void selectNextOccurrence();
    void foldCode();
    void unfoldCode();
    void foldAllCode();
    void unfoldAllCode();
    void cancelOperation();
    void insertNewline();
    void insertTab();
    void insertBacktab();
    void insertText(const std::string& text);
    bool canUndo() const;
    bool canRedo() const;
    bool hasLineComment() const { return !configuration_.lineComment.empty(); }

    // ---- Callbacks
    std::function<void()> onSelectionChange;
    std::function<void()> onEdit;
    /// After every edit: what was typed, or nothing for a deletion or other change.
    std::function<void(const std::optional<std::string>& typed)> onTypedText;
    std::function<void()> onSectionsChange;
    /// The pointer rested on a word starting at `position`.
    std::function<void(Position position, Range word)> onHoverRequest;
    std::function<void()> onHoverEnd;
    /// "up", "down", "pageup", "pagedown", "enter", "tab", "escape": true if the completion list used it.
    std::function<bool(const std::string& action)> onCompletionKey;
    std::function<void()> onBreakpointsChange;
    /// A change bar was clicked; `bar` is in window coordinates.
    std::function<void(size_t line, ui::Rect bar)> onChangeMarkerClick;
    std::function<void()> onVimStatusChange;
    /// A ":" command the editor can't do itself; returns an error message, or "".
    std::function<std::string(const std::string& command)> onVimCommand;
    std::function<void(VimAction action)> onVimAction;
    /// Right-click at (x, y) in window coordinates.
    std::function<void(int x, int y)> onContextMenu;
    /// Something about the editor needs drawing.
    std::function<void()> onNeedsDisplay;

private:
    struct Chip {
        int x = 0;
        int width = 0;
        std::string label;
    };
    struct Highlight {
        size_t start = 0;
        size_t end = 0;
        Color color = 0;
    };
    /// One line laid out in character cells.
    struct LineLayout {
        size_t rowCount() const { return rowStarts.size(); }
        size_t rowForByte(size_t byte) const;
        /// Pixels from the text's left edge.
        int xForByte(size_t byte) const;

        std::vector<int> x;  // per byte, plus one for the end: pixels from the row's start
        std::vector<uint32_t> rowStarts{0};
        int continuationIndent = 0;
        std::vector<Color> colors;  // per byte
        std::vector<uint8_t> underline;  // per byte: 1 underline, 2 strikethrough
        std::vector<Highlight> highlights;
        std::vector<Chip> chips;
        bool highlighted = false;
    };
    struct LineDecoration {
        Decoration look;
        size_t startByte = 0;
        size_t endByte = 0;
        bool toEndOfLine = false;
        bool showsNote = false;
    };

    // ---- Geometry
    int textLeft() const;
    int ui(double points) const;
    ui::Rect textArea() const;
    LineLayout& layoutForLine(size_t line);
    double yForLine(size_t line) const;
    uint32_t rowsForLine(size_t line) const;
    bool isLineHidden(size_t line) const;
    size_t lineAtY(double y, size_t* subrow) const;
    double xForPosition(Position position);
    double yForPosition(Position position);
    Position positionForPoint(double x, double y);
    /// Window point to the text's own coordinates (scrolled).
    std::pair<double, double> documentPoint(int x, int y) const;
    template <typename Body>
    void forEachSegment(size_t line, size_t startByte, size_t endByte, bool continues, Body&& body);
    uint32_t computeWrapColumns() const;
    void rebuildRowMap();
    void rowLayoutChanged();
    void clampScroll();
    void needsDisplay();

    // ---- Drawing
    void drawGutter(ui::Canvas& canvas, size_t first, size_t last);
    void drawText(ui::Canvas& canvas, size_t first, size_t last, bool focused);
    void drawCaret(ui::Canvas& canvas, bool focused);
    void drawSquiggle(ui::Canvas& canvas, int left, int right, int top, Color color);
    void drawInlineMessage(ui::Canvas& canvas, const std::string& message, int severity, size_t line);
    Color colorForSeverity(int severity) const;
    int diagnosticSeverityForLine(size_t line) const;
    std::optional<std::pair<Position, Position>> bracketMatch();
    std::vector<bool> codeMaskForLine(size_t line, const std::vector<tm::Token>* tokens);
    bool isInert(const tm::ScopeNode* scopes);

    // ---- Caret and smear
    ui::Rect caretRect();
    ui::Rect caretLayerRect();
    bool vimBlockCursor() const;
    void updateCaret(bool animated);
    void stopSmear();

    // ---- Editing helpers
    void didEdit();
    void moveCaretTo(Position position, bool extend);
    void moveHorizontally(int direction, bool byWord, bool extend);
    void moveVertically(long rows, bool extend);
    void moveToLineStart(bool extend);
    void moveToLineEnd(bool extend);
    long pageLines() const;
    void deleteBackward();
    void deleteForward();
    void deleteWordBackward();
    void deleteWordForward();
    std::string indentUnit() const;
    std::string textAround(Position position, bool before) const;
    std::pair<size_t, size_t> selectedLines() const;
    void transformLines(const std::function<std::string(const std::string&, size_t)>& transform);
    void transformSelectedText(std::string (*transform)(std::string_view), bool wholeLines);
    std::string joinedTextOfLines(size_t first, size_t last) const;
    std::string copyTextOfSelection(const Selection& selection) const;
    void deleteSelectionOrLineForCut();
    std::vector<Selection> selectionsInOrder() const;
    void normalizeCursors();
    void forEachSelection(const std::function<void()>& body, bool mergingLines);
    void addCursorInDirection(long direction);
    void selectColumn(double startX, double startY, double endX, double endY);
    void toggleFoldAtLine(size_t line);
    void moveCaretOutOfHiddenLines();
    std::optional<size_t> foldableLineContaining(size_t line);
    bool isLineFoldable(size_t line) const;
    void refreshSections();
    void cancelHover();
    bool handleVimKey(const std::string& key);
    void vimStatusDidChange();

    // ---- State
    ui::Rect frame_;
    Document* document_ = nullptr;
    Highlighter* highlighter_ = nullptr;
    LanguageConfiguration configuration_;
    std::shared_ptr<ui::Font> font_;
    int charWidth_ = 12;
    int lineHeight_ = 24;
    int baseline_ = 18;
    int pixel_ = 2;
    bool crisp_ = true;
    size_t tabSize_ = 4;
    size_t tabSizeOverride_ = 0;
    int useTabsOverride_ = -1;
    bool useTabs_ = false;
    bool detectedUseTabs_ = false;
    bool wrapEnabled_ = false;
    uint32_t wrapColumns_ = 0;
    double scrollX_ = 0;
    double scrollY_ = 0;
    size_t longestLine_ = 0;
    std::unordered_map<size_t, LineLayout> layouts_;
    bool highlightBlocked_ = false;
    uint64_t continueTimer_ = 0;
    RowMap rowMap_;
    FoldSet folds_;
    std::vector<Section> sections_;
    uint64_t sectionsTimer_ = 0;

    // Colors.
    Color background_ = 0;
    Color foreground_ = 0;
    Color selectionColor_ = 0;
    Color lineHighlight_ = 0;
    Color caretColor_ = 0;
    Color gutterColor_ = 0;
    Color lineNumberColor_ = 0;
    Color activeLineNumberColor_ = 0;
    Color matchColor_ = 0;
    Color occurrenceColor_ = 0;
    Color findColor_ = 0;
    Color currentFindColor_ = 0;
    Color dimColor_ = 0;

    // Marks.
    std::vector<LineChange> lineChanges_;
    std::vector<Color> lineTints_;
    std::vector<Diagnostic> diagnostics_;
    std::map<size_t, std::vector<InlayHint>> inlayByLine_;
    std::map<size_t, std::vector<LineDecoration>> decorationsByLine_;
    SemanticTokenSet semantic_;
    std::unordered_map<uint32_t, std::optional<TextStyle>> semanticStyles_;
    std::vector<Range> findMatches_;
    long currentFindMatch_ = -1;
    long executionLine_ = -1;
    std::unordered_map<const tm::ScopeNode*, bool> inertScopes_;
    uint64_t matchVersion_ = UINT64_MAX;
    Position matchCaret_;
    std::optional<std::pair<Position, Position>> match_;
    struct Flash {
        size_t first;
        size_t count;
        double start;
    };
    std::vector<Flash> flashes_;

    // Caret, blinking and the smear.
    bool focused_ = false;
    double blinkStart_ = 0;
    ui::Rect lastCaretRect_;
    bool hasLastCaretRect_ = false;
    bool lastCaretWasBlock_ = false;
    SpringSmear smear_;
    bool smearing_ = false;
    double lastSmearFrame_ = 0;

    // Mouse.
    enum class Drag { None, Text, Gutter, Column };
    Drag drag_ = Drag::None;
    Position dragAnchor_;
    int dragClicks_ = 1;
    double columnStartX_ = 0;
    double columnStartY_ = 0;
    bool gutterHover_ = false;
    Range hoverWord_{{SIZE_MAX, 0}, {SIZE_MAX, 0}};
    bool hoverActive_ = false;
    uint64_t hoverTimer_ = 0;

    // Multiple cursors and commands.
    int multiDepth_ = 0;
    bool multiEdited_ = false;
    bool multiMoved_ = false;
    size_t cursorIndexInLoop_ = 0;
    std::optional<std::string> pendingTyped_;
    std::vector<Selection> expandHistory_;
    Selection lastExpanded_;

    // Vim.
    VimEngine vim_;
    std::string vimShownStatus_;
};

}  // namespace gw::app
