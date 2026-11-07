#include "EditorView.hpp"

#include "Loop.hpp"
#include "PixelArt.hpp"
#include "Platform.hpp"
#include "SelectionExpand.hpp"
#include "Services.hpp"
#include "Styling.hpp"
#include "TextTransforms.hpp"
#include "Wrap.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace gw::app {

/// Commands acting at the caret run once per cursor when there are several (see forEachSelection).
#define FOR_EACH_CURSOR(call)                                                     \
    if (multiDepth_ == 0 && document_ && !document_->extraSelections.empty()) {  \
        forEachSelection([&] { call; }, false);                                  \
        return;                                                                  \
    }
/// The same for commands that act on whole lines: cursors on one line count once.
#define FOR_EACH_CURSOR_LINE(call)                                                \
    if (multiDepth_ == 0 && document_ && !document_->extraSelections.empty()) {  \
        forEachSelection([&] { call; }, true);                                   \
        return;                                                                  \
    }

namespace {

/// Lines of tokenizing allowed per call while drawing; beyond it, lines draw plain and the
/// highlighter catches up over the next frames instead of stalling.
constexpr size_t kHighlightBudget = 400;
constexpr double kBlinkPeriod = 0.55;

bool isOpenerByte(char c) { return c == '(' || c == '[' || c == '{'; }
bool isCloserByte(char c) { return c == ')' || c == ']' || c == '}'; }

char matchingByte(char c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        default: return 0;
    }
}

bool isOpeningBracket(const std::string& text) { return text == "{" || text == "[" || text == "("; }

bool selectionsOverlap(const Selection& a, const Selection& b) {
    Range first = a.range();
    Range second = b.range();
    if (first.start < second.end && second.start < first.end) {
        return true;
    }
    if (a.empty() && second.start <= first.start && first.start <= second.end) {
        return true;
    }
    if (b.empty() && first.start <= second.start && second.start <= first.end) {
        return true;
    }
    return false;
}

bool sameSelection(const Selection& a, const Selection& b) {
    return a.anchor == b.anchor && a.caret == b.caret;
}

std::string sortAscending(std::string_view text) { return sortLinesText(text, false); }
std::string sortDescending(std::string_view text) { return sortLinesText(text, true); }

/// UTF-16 units before each byte of a line (language servers count in UTF-16).
std::vector<uint32_t> unitsOfBytes(std::string_view text) {
    std::vector<uint32_t> units(text.size() + 1, 0);
    uint32_t unit = 0;
    size_t index = 0;
    while (index < text.size()) {
        unsigned char first = static_cast<unsigned char>(text[index]);
        size_t length = first < 0x80 ? 1 : first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
        for (size_t step = 0; step < length && index + step < text.size(); ++step) {
            units[index + step] = unit;
        }
        unit += length == 4 ? 2 : 1;
        index += length;
    }
    units[text.size()] = unit;
    return units;
}

}  // namespace

// ---------------------------------------------------------------------------
// Line layouts

size_t EditorView::LineLayout::rowForByte(size_t byte) const {
    if (rowStarts.size() <= 1) {
        return 0;
    }
    auto found = std::upper_bound(rowStarts.begin(), rowStarts.end(), static_cast<uint32_t>(byte));
    return static_cast<size_t>(std::max<long>(0, (found - rowStarts.begin()) - 1));
}

int EditorView::LineLayout::xForByte(size_t byte) const {
    if (x.empty()) {
        return 0;
    }
    return x[std::min(byte, x.size() - 1)];
}

EditorView::EditorView() {
    reloadAppearance();
    // Vim's yanks and puts use the system clipboard, and hands the window what it can't do itself.
    vim_.readClipboard = [] { return clipboardText(); };
    vim_.writeClipboard = [](const std::string& text) { setClipboardText(text); };
    vim_.onAction = [this](VimAction action) {
        if (onVimAction) {
            onVimAction(action);
        }
    };
    vim_.onCommand = [this](const std::string& command) {
        return onVimCommand ? onVimCommand(command) : "E492: Not an editor command: " + command;
    };
}

int EditorView::ui(double points) const {
    return static_cast<int>(std::lround(points * Services::shared().displayScale()));
}

int EditorView::textLeft() const { return ui(14); }

int EditorView::gutterWidth() const {
    size_t count = document_ ? document_->buffer().lineCount() : 1;
    int digits = std::max(3, static_cast<int>(std::log10(static_cast<double>(std::max<size_t>(count, 1)))) + 1);
    return digits * charWidth_ + ui(40);
}

ui::Rect EditorView::textArea() const {
    int gutter = gutterWidth();
    return {frame_.x + gutter, frame_.y, std::max(0, frame_.w - gutter), frame_.h};
}

int EditorView::visibleWidth() const { return textArea().w; }

void EditorView::needsDisplay() {
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
}

// ---------------------------------------------------------------------------
// Setup

void EditorView::setDocument(Document* document, Highlighter* highlighter, const LanguageConfiguration& configuration) {
    vim_.reset(document_);
    stopSmear();
    hasLastCaretRect_ = false;
    document_ = document;
    highlighter_ = highlighter;
    configuration_ = configuration;
    folds_.clear();
    lineChanges_.clear();
    lineTints_.clear();
    findMatches_.clear();
    currentFindMatch_ = -1;
    diagnostics_.clear();
    inlayByLine_.clear();
    decorationsByLine_.clear();
    semantic_ = SemanticTokenSet();
    semanticStyles_.clear();
    executionLine_ = -1;
    useTabs_ = false;
    matchVersion_ = UINT64_MAX;
    scrollX_ = scrollY_ = 0;
    expandHistory_.clear();
    if (document) {
        // Indent with tabs if the file already mostly does.
        size_t tabs = 0, spaces = 0;
        const auto& buffer = document->buffer();
        for (size_t line = 0; line < std::min<size_t>(buffer.lineCount(), 2000); ++line) {
            std::string_view text = buffer.line(line);
            if (!text.empty() && text[0] == '\t') {
                ++tabs;
            }
            if (text.size() >= 2 && text[0] == ' ' && text[1] == ' ') {
                ++spaces;
            }
        }
        detectedUseTabs_ = tabs > spaces;
        useTabs_ = useTabsOverride_ >= 0 ? useTabsOverride_ == 1 : detectedUseTabs_;
    }
    refreshSections();
    invalidateLayout();
    vimStatusDidChange();
    updateCaret(false);
    needsDisplay();
}

void EditorView::setIndentationOverride(int usesTabs, size_t tabSize) {
    bool changedSize = tabSize != tabSizeOverride_;
    useTabsOverride_ = usesTabs;
    tabSizeOverride_ = tabSize;
    useTabs_ = usesTabs >= 0 ? usesTabs == 1 : detectedUseTabs_;
    if (changedSize) {
        reloadAppearance();
    }
}

void EditorView::refreshSections() {
    if (document_) {
        sections_ = findSections(document_->buffer());
    } else {
        sections_.clear();
    }
    if (onSectionsChange) {
        onSectionsChange();
    }
}

void EditorView::reloadAppearance() {
    Services& services = Services::shared();
    const EditorSettings& settings = services.settings();
    tabSize_ = tabSizeOverride_ > 0 ? tabSizeOverride_ : static_cast<size_t>(std::max(1, settings.tabSize));
    bool wrap = settings.wordWrap;
    if (settings.vimMode && !vim_.inVisualMode() && document_ == nullptr) {
        vim_.reset(nullptr);
    }
    // A new size changes every row height: keep the top line where it is.
    size_t topLine = document_ && lineHeight_ > 0 ? lineAtY(scrollY_, nullptr) : 0;
    font_ = services.editorFont();
    crisp_ = font_->isPixelFont();
    charWidth_ = font_->cellWidth();
    lineHeight_ = font_->lineHeight();
    baseline_ = font_->baseline();
    pixel_ = std::max(1, font_->pixel());
    wrapEnabled_ = wrap;

    const bitmap::Scheme& scheme = services.scheme();
    const Theme& theme = services.theme();
    background_ = (theme.color("editor.background", scheme.window) & 0xFFFFFF00u) | 0xFF;
    foreground_ = theme.color("editor.foreground", scheme.text);
    selectionColor_ = theme.color("editor.selectionBackground", scheme.selection);
    lineHighlight_ = theme.color("editor.lineHighlightBackground", 0xFFFFFF10);
    caretColor_ = theme.color("editorCursor.foreground", scheme.text);
    matchColor_ = theme.color("editorBracketMatch.border", scheme.text);
    occurrenceColor_ = theme.color("editor.wordHighlightBackground", 0x5555FF60);
    findColor_ = theme.color("editor.findMatchHighlightBackground", 0xAA550080);
    currentFindColor_ = theme.color("editor.findMatchBackground", 0xAA5500FF);
    gutterColor_ = scheme.gutter;
    lineNumberColor_ = scheme.lineNumber;
    activeLineNumberColor_ = scheme.lineNumberActive;
    dimColor_ = scheme.textDim;
    semanticStyles_.clear();
    wrapColumns_ = 0;
    invalidateLayout();
    if (document_) {
        scrollY_ = yForLine(std::min(topLine, document_->buffer().lineCount() - 1));
        clampScroll();
    }
    updateCaret(false);
    vimStatusDidChange();
    needsDisplay();
}

void EditorView::invalidateLayout() {
    layouts_.clear();
    rebuildRowMap();
    longestLine_ = 0;
    if (document_) {
        const auto& buffer = document_->buffer();
        for (size_t line = 0; line < buffer.lineCount(); ++line) {
            longestLine_ = std::max(longestLine_, buffer.line(line).size());
        }
    }
    needsDisplay();
}

void EditorView::setFrame(const ui::Rect& frame) {
    if (frame == frame_) {
        return;
    }
    bool widthChanged = frame.w != frame_.w;
    frame_ = frame;
    if (widthChanged && wrapEnabled_) {
        wrapColumns_ = 0;
        layouts_.clear();
        rebuildRowMap();
    }
    clampScroll();
}

// ---------------------------------------------------------------------------
// Rows (soft wrap and folding)

double EditorView::yForLine(size_t line) const {
    if (rowMap_.empty()) {
        return static_cast<double>(line) * lineHeight_;
    }
    return static_cast<double>(rowMap_.firstRow(std::min(line, rowMap_.lineCount()))) * lineHeight_;
}

uint32_t EditorView::rowsForLine(size_t line) const {
    if (rowMap_.empty() || line >= rowMap_.lineCount()) {
        return 1;
    }
    return rowMap_.rows(line);
}

bool EditorView::isLineHidden(size_t line) const {
    if (rowMap_.empty() || line >= rowMap_.lineCount()) {
        return false;
    }
    return rowMap_.rows(line) == 0;
}

size_t EditorView::lineAtY(double y, size_t* subrow) const {
    if (y < 0) {
        y = 0;
    }
    uint64_t row = static_cast<uint64_t>(y / std::max(1, lineHeight_));
    if (rowMap_.empty()) {
        if (subrow != nullptr) {
            *subrow = 0;
        }
        return static_cast<size_t>(row);
    }
    if (row >= rowMap_.totalRows()) {
        if (subrow != nullptr) {
            *subrow = 0;
        }
        return rowMap_.lineCount();
    }
    size_t line = rowMap_.lineForRow(row);
    if (subrow != nullptr) {
        *subrow = static_cast<size_t>(row - rowMap_.firstRow(line));
    }
    return line;
}

uint32_t EditorView::computeWrapColumns() const {
    int usable = visibleWidth() - textLeft() - ui(24);
    if (charWidth_ <= 0) {
        return 80;
    }
    return static_cast<uint32_t>(std::max(8, usable / charWidth_));
}

void EditorView::rebuildRowMap() {
    if (!document_ || (!wrapEnabled_ && folds_.empty())) {
        rowMap_.clear();
        return;
    }
    const auto& buffer = document_->buffer();
    size_t count = buffer.lineCount();
    std::vector<uint32_t> rows(count, 1);
    if (wrapEnabled_) {
        if (wrapColumns_ == 0) {
            wrapColumns_ = computeWrapColumns();
        }
        for (size_t line = 0; line < count; ++line) {
            rows[line] = countWrapRows(buffer.line(line), wrapColumns_, static_cast<uint32_t>(tabSize_));
        }
    }
    for (const Fold& fold : folds_.all()) {
        for (size_t line = fold.first + 1; line <= fold.last && line < count; ++line) {
            rows[line] = 0;
        }
    }
    rowMap_.assign(std::move(rows));
}

void EditorView::rowLayoutChanged() {
    layouts_.clear();
    rebuildRowMap();
    clampScroll();
    updateCaret(false);
    needsDisplay();
}

void EditorView::documentChanged(Document* document, const Change& change) {
    if (document != document_) {
        return;
    }
    matchVersion_ = UINT64_MAX;
    size_t foldCountBefore = folds_.all().size();
    if (foldCountBefore > 0) {
        folds_.adjustForEdit(change.firstLine, change.lastLine, change.lineDelta);
    }
    if (!semantic_.tokens.empty()) {
        size_t lastLineBefore = change.lastLine >= static_cast<size_t>(std::max(0L, change.lineDelta))
                                    ? static_cast<size_t>(static_cast<long>(change.lastLine) - change.lineDelta)
                                    : change.firstLine;
        adjustSemanticTokensForEdit(semantic_.tokens, change.firstLine, std::max(change.firstLine, lastLineBefore), change.lineDelta);
    }
    // Problems and hints move with their lines until the server sends new ones.
    if (change.lineDelta != 0) {
        for (Diagnostic& diagnostic : diagnostics_) {
            if (diagnostic.range.start.line > change.lastLine) {
                diagnostic.range.start.line = static_cast<size_t>(static_cast<long>(diagnostic.range.start.line) + change.lineDelta);
                diagnostic.range.end.line = static_cast<size_t>(static_cast<long>(diagnostic.range.end.line) + change.lineDelta);
            }
        }
        std::map<size_t, std::vector<InlayHint>> moved;
        for (auto& [line, hints] : inlayByLine_) {
            if (line < change.firstLine) {
                moved[line] = std::move(hints);
            } else if (line > change.lastLine) {
                moved[static_cast<size_t>(static_cast<long>(line) + change.lineDelta)] = std::move(hints);
            }
        }
        inlayByLine_ = std::move(moved);
    }
    if (!wrapEnabled_ && folds_.empty() && rowMap_.empty()) {
        return;
    }
    bool foldsChanged = folds_.all().size() != foldCountBefore;
    const auto& buffer = document->buffer();
    size_t expectedOldCount = static_cast<size_t>(static_cast<long>(buffer.lineCount()) - change.lineDelta);
    bool mapMatchesEdit = rowMap_.lineCount() == expectedOldCount;
    if (foldsChanged || !folds_.empty() || !mapMatchesEdit) {
        rebuildRowMap();
        return;
    }
    if (change.lineDelta != 0) {
        size_t touchedNow = change.lastLine - change.firstLine + 1;
        size_t touchedBefore = static_cast<size_t>(static_cast<long>(touchedNow) - change.lineDelta);
        std::vector<uint32_t> newRows;
        for (size_t line = change.firstLine; line <= change.lastLine && line < buffer.lineCount(); ++line) {
            newRows.push_back(wrapEnabled_ ? countWrapRows(buffer.line(line), wrapColumns_, static_cast<uint32_t>(tabSize_)) : 1);
        }
        rowMap_.replaceLines(change.firstLine, touchedBefore, newRows);
        return;
    }
    if (wrapEnabled_) {
        size_t last = std::min(change.lastLine, buffer.lineCount() - 1);
        for (size_t line = change.firstLine; line <= last; ++line) {
            if (rowMap_.rows(line) != 0) {
                rowMap_.setRows(line, countWrapRows(buffer.line(line), wrapColumns_, static_cast<uint32_t>(tabSize_)));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Layout

bool EditorView::isInert(const tm::ScopeNode* scopes) {
    auto found = inertScopes_.find(scopes);
    if (found != inertScopes_.end()) {
        return found->second;
    }
    bool inert = false;
    for (const tm::ScopeNode* node = scopes; node && !inert; node = node->parent) {
        inert = node->name.rfind("string", 0) == 0 || node->name.rfind("comment", 0) == 0;
    }
    inertScopes_[scopes] = inert;
    return inert;
}

std::vector<bool> EditorView::codeMaskForLine(size_t line, const std::vector<tm::Token>* tokens) {
    std::string_view text = document_->buffer().line(line);
    std::vector<bool> mask(text.size(), true);
    if (tokens) {
        for (const auto& token : *tokens) {
            if (!isInert(token.scopes)) {
                continue;
            }
            for (size_t index = token.start; index < token.end && index < mask.size(); ++index) {
                mask[index] = false;
            }
        }
    }
    return mask;
}

EditorView::LineLayout& EditorView::layoutForLine(size_t index) {
    const auto& buffer = document_->buffer();
    index = std::min(index, buffer.lineCount() - 1);
    const std::vector<tm::Token>* tokens = nullptr;
    if (highlighter_ && highlighter_->grammar() && !highlightBlocked_) {
        tokens = highlighter_->tokens(buffer, index, kHighlightBudget);
        if (!tokens) {
            highlightBlocked_ = true;
            if (continueTimer_ == 0) {
                continueTimer_ = afterDelay(0.01, [this] {
                    continueTimer_ = 0;
                    needsDisplay();
                });
            }
        }
    }
    auto found = layouts_.find(index);
    if (found != layouts_.end() && (found->second.highlighted || !tokens)) {
        return found->second;
    }

    std::string_view text = buffer.line(index);
    LineLayout layout;
    layout.colors.assign(text.size(), foreground_);
    layout.underline.assign(text.size(), 0);
    Services& services = Services::shared();
    if (tokens) {
        for (const StyledSpan& span : services.stylist().styleLine(text, *tokens)) {
            size_t end = std::min<size_t>(span.end, text.size());
            for (size_t byte = span.start; byte < end; ++byte) {
                layout.colors[byte] = span.style.foreground;
                if (span.style.underline) {
                    layout.underline[byte] = 1;
                }
                if (span.style.strikethrough) {
                    layout.underline[byte] = 2;
                }
            }
            if (span.style.background) {
                layout.highlights.push_back({span.start, end, *span.style.background});
            }
        }
        layout.highlighted = true;
    }
    // Semantic tokens: the language server's knowledge, over the grammar's colors.
    if (!semantic_.tokens.empty() && services.settings().semanticHighlighting) {
        auto first = std::lower_bound(semantic_.tokens.begin(), semantic_.tokens.end(), index,
                                      [](const SemanticToken& token, size_t value) { return token.line < value; });
        if (first != semantic_.tokens.end() && first->line == index) {
            std::vector<uint32_t> units = unitsOfBytes(text);
            for (auto token = first; token != semantic_.tokens.end() && token->line == index; ++token) {
                std::optional<TextStyle> style;
                auto cached = semanticStyles_.find(token->type);
                if (cached != semanticStyles_.end()) {
                    style = cached->second;
                } else {
                    if (token->type < semantic_.typeNames.size()) {
                        std::string scope = scopeForSemanticType(semantic_.typeNames[token->type], semantic_.modifierNames);
                        if (!scope.empty()) {
                            const Theme& theme = services.theme();
                            TextStyle candidate = theme.styleFor(std::vector<std::string>{"source", scope});
                            if (candidate.foreground != theme.color("editor.foreground", 0xD4D4D4FF)) {
                                style = candidate;
                            }
                        }
                    }
                    semanticStyles_[token->type] = style;
                }
                if (!style) {
                    continue;
                }
                for (size_t byte = 0; byte < text.size(); ++byte) {
                    if (units[byte] >= token->startUnit && units[byte] < token->startUnit + token->lengthUnits) {
                        layout.colors[byte] = style->foreground;
                    }
                }
            }
        }
    }
    // Extension decorations that color text.
    if (auto decorated = decorationsByLine_.find(index); decorated != decorationsByLine_.end()) {
        for (const LineDecoration& entry : decorated->second) {
            if (!entry.look.foreground && !entry.look.underline && !entry.look.strikethrough) {
                continue;
            }
            size_t start = std::min(entry.startByte, text.size());
            size_t end = entry.toEndOfLine ? text.size() : std::min(entry.endByte, text.size());
            for (size_t byte = start; byte < end; ++byte) {
                if (entry.look.foreground) {
                    layout.colors[byte] = *entry.look.foreground;
                }
                if (entry.look.underline) {
                    layout.underline[byte] = 1;
                }
                if (entry.look.strikethrough) {
                    layout.underline[byte] = 2;
                }
            }
        }
    }

    // Positions: characters take cells (tabs to the next stop, wide characters two); inlay hints
    // open gaps without adding characters, so every position in the line stays valid.
    std::map<size_t, int> gapBefore;
    std::vector<std::pair<size_t, const InlayHint*>> hints;
    if (!wrapEnabled_) {
        if (auto found = inlayByLine_.find(index); found != inlayByLine_.end()) {
            for (const InlayHint& hint : found->second) {
                size_t column = hint.position.column;
                if (column == 0 || column > text.size()) {
                    continue;
                }
                int width = (ui::textCells(hint.label) + 1) * charWidth_;
                if (column < text.size()) {
                    gapBefore[column] += width;
                }
                hints.emplace_back(column, &hint);
            }
        }
    }
    if (wrapEnabled_) {
        if (wrapColumns_ == 0) {
            wrapColumns_ = computeWrapColumns();
        }
        WrapResult wrapped = wrapLine(text, wrapColumns_, static_cast<uint32_t>(tabSize_));
        if (wrapped.rowStarts.size() > 1) {
            layout.rowStarts = wrapped.rowStarts;
            layout.continuationIndent = static_cast<int>(wrapped.continuationIndent) * charWidth_;
        }
    }
    layout.x.assign(text.size() + 1, 0);
    size_t row = 0;
    int cells = 0;
    int extra = 0;
    for (size_t byte = 0; byte < text.size();) {
        if (row + 1 < layout.rowStarts.size() && byte >= layout.rowStarts[row + 1]) {
            ++row;
            cells = 0;
            extra = layout.continuationIndent;
        }
        if (auto gap = gapBefore.find(byte); gap != gapBefore.end()) {
            extra += gap->second;
        }
        size_t next = byte;
        char32_t character = ui::decodeUtf8(text, next);
        int x = cells * charWidth_ + extra;
        for (size_t step = byte; step < next; ++step) {
            layout.x[step] = x;
        }
        if (character == '\t') {
            cells = (cells / static_cast<int>(tabSize_) + 1) * static_cast<int>(tabSize_);
        } else {
            cells += ui::Font::cells(character);
        }
        byte = next;
    }
    layout.x[text.size()] = cells * charWidth_ + extra;
    // The hints' chips, in the gaps opened for them (or after the end of the line).
    std::map<size_t, int> used;
    for (const auto& [column, hint] : hints) {
        int width = (ui::textCells(hint->label) + 1) * charWidth_;
        int x;
        if (column >= text.size()) {
            x = layout.x[text.size()] + charWidth_ / 2 + used[column];
        } else {
            x = layout.x[column] - gapBefore[column] + used[column];
        }
        used[column] += width;
        layout.chips.push_back({x, width, hint->label});
    }
    auto inserted = layouts_.insert_or_assign(index, std::move(layout));
    return inserted.first->second;
}

double EditorView::xForPosition(Position position) {
    LineLayout& layout = layoutForLine(position.line);
    return textLeft() + layout.xForByte(position.column);
}

double EditorView::yForPosition(Position position) {
    LineLayout& layout = layoutForLine(position.line);
    return yForLine(position.line) + layout.rowForByte(position.column) * lineHeight_;
}

Position EditorView::positionForPoint(double x, double y) {
    const auto& buffer = document_->buffer();
    if (y < 0) {
        return {0, 0};
    }
    size_t subrow = 0;
    size_t line = lineAtY(y, &subrow);
    if (line >= buffer.lineCount()) {
        return buffer.end();
    }
    LineLayout& layout = layoutForLine(line);
    subrow = std::min(subrow, layout.rowCount() - 1);
    std::string_view text = buffer.line(line);
    size_t rowStart = layout.rowStarts[subrow];
    size_t rowEnd = subrow + 1 < layout.rowCount() ? layout.rowStarts[subrow + 1] : text.size();
    double local = x - textLeft();
    // The character whose left half or right half is under the point.
    size_t best = rowStart;
    for (size_t byte = rowStart; byte < rowEnd;) {
        size_t next = byte;
        ui::decodeUtf8(text, next);
        double left = layout.x[byte];
        double right = next < layout.x.size() ? layout.x[next] : left + charWidth_;
        if (next >= rowEnd && subrow + 1 < layout.rowCount()) {
            right = left + charWidth_;
        }
        if (local < (left + right) / 2) {
            best = byte;
            break;
        }
        best = next;
        byte = next;
    }
    if (subrow + 1 < layout.rowCount()) {
        // Past the end of a wrapped row: the caret stays at its end, not on the next row.
        best = std::min(best, rowEnd > rowStart ? rowEnd - 1 : rowEnd);
        best = floorToCharacter(text, best);
    }
    return buffer.clamp({line, best});
}

std::pair<double, double> EditorView::documentPoint(int x, int y) const {
    ui::Rect area = textArea();
    return {x - area.x + scrollX_, y - frame_.y + scrollY_};
}

template <typename Body>
void EditorView::forEachSegment(size_t line, size_t startByte, size_t endByte, bool continues, Body&& body) {
    LineLayout& layout = layoutForLine(line);
    double lineTop = yForLine(line);
    size_t rows = layout.rowCount();
    size_t size = layout.x.empty() ? 0 : layout.x.size() - 1;
    startByte = std::min(startByte, size);
    endByte = std::min(endByte, size);
    if (startByte >= endByte) {
        size_t row = layout.rowForByte(startByte);
        double x = textLeft() + layout.xForByte(startByte);
        body(x, continues && row == rows - 1 ? x + charWidth_ * 0.6 : x, lineTop + row * lineHeight_);
        return;
    }
    for (size_t row = 0; row < rows; ++row) {
        size_t rowStart = layout.rowStarts[row];
        size_t rowEnd = row + 1 < rows ? layout.rowStarts[row + 1] : size;
        size_t from = std::max(startByte, rowStart);
        size_t to = std::min(endByte, rowEnd);
        if (from >= to) {
            continue;
        }
        double left = textLeft() + layout.xForByte(from);
        double right;
        if (row + 1 < rows && to == rowEnd) {
            // The row's last character ends where its own width does, not at the next row's start.
            size_t last = floorToCharacter(document_->buffer().line(line), to - 1);
            right = textLeft() + layout.xForByte(last) + charWidth_;
        } else {
            right = textLeft() + layout.xForByte(to);
        }
        if (row == rows - 1 && to == endByte && continues) {
            right += charWidth_ * 0.6;
        }
        body(left, right, lineTop + row * lineHeight_);
    }
}

double EditorView::contentHeight() const {
    if (!document_) {
        return 0;
    }
    double rows = rowMap_.empty() ? static_cast<double>(document_->buffer().lineCount()) : static_cast<double>(rowMap_.totalRows());
    // Room to scroll the last line up to the middle.
    return rows * lineHeight_ + std::max(0, frame_.h / 2);
}

double EditorView::contentWidth() const {
    if (!document_ || wrapEnabled_) {
        return visibleWidth();
    }
    return std::max<double>(visibleWidth(), textLeft() + static_cast<double>(longestLine_) * charWidth_ + ui(80));
}

void EditorView::clampScroll() {
    double maxY = std::max(0.0, contentHeight() - frame_.h);
    double maxX = std::max(0.0, contentWidth() - visibleWidth());
    scrollY_ = std::clamp(scrollY_, 0.0, maxY);
    scrollX_ = std::clamp(scrollX_, 0.0, maxX);
}

void EditorView::scrollTo(double x, double y) {
    double oldX = scrollX_, oldY = scrollY_;
    scrollX_ = x;
    scrollY_ = y;
    clampScroll();
    if (oldX != scrollX_ || oldY != scrollY_) {
        cancelHover();
        stopSmear();
        updateCaret(false);
        needsDisplay();
    }
}

// ---------------------------------------------------------------------------
// Drawing

Color EditorView::colorForSeverity(int severity) const {
    const bitmap::Scheme& colors = Services::shared().scheme();
    return severity == 1 ? colors.error : severity == 2 ? colors.warning : colors.info;
}

int EditorView::diagnosticSeverityForLine(size_t line) const {
    auto begin = std::lower_bound(diagnostics_.begin(), diagnostics_.end(), line,
                                  [](const Diagnostic& item, size_t value) { return item.range.start.line < value; });
    if (begin == diagnostics_.end() || begin->range.start.line != line) {
        return 0;
    }
    return begin->severity;  // sorted: the first one is the most severe
}

void EditorView::draw(ui::Canvas& canvas, bool focused) {
    focused_ = focused;
    canvas.pushClip(frame_);
    canvas.fill(frame_, background_);
    if (!document_) {
        canvas.popClip();
        return;
    }
    highlightBlocked_ = false;
    const auto& buffer = document_->buffer();
    size_t count = buffer.lineCount();
    size_t first = lineAtY(scrollY_, nullptr);
    size_t last = std::min(count - 1, lineAtY(scrollY_ + frame_.h, nullptr));
    if (first < count) {
        drawText(canvas, first, last, focused);
        drawGutter(canvas, first, last);
    } else {
        canvas.fill({frame_.x, frame_.y, gutterWidth(), frame_.h}, gutterColor_);
    }
    canvas.popClip();
}

void EditorView::drawGutter(ui::Canvas& canvas, size_t first, size_t last) {
    int width = gutterWidth();
    ui::Rect gutter{frame_.x, frame_.y, width, frame_.h};
    canvas.fill(gutter, gutterColor_);
    canvas.pushClip(gutter);
    size_t caretLine = document_->selection.caret.line;
    ui::Font& font = *font_;
    int chromePixel = Services::shared().chromePixel();
    for (size_t line = first; line <= last; ++line) {
        if (isLineHidden(line)) {
            continue;
        }
        int top = frame_.y + static_cast<int>(std::lround(yForLine(line) - scrollY_));
        std::string number = std::to_string(line + 1);
        int right = frame_.x + width - ui(26);
        int numberWidth = ui::textWidth(font, number);
        Color color = line == caretLine ? activeLineNumberColor_ : lineNumberColor_;
        ui::drawText(canvas, font, right - numberWidth, top, number, color, true);
        bool hasBreakpoint = document_->breakpointLines.count(line) > 0;
        bool isExecution = executionLine_ >= 0 && static_cast<size_t>(executionLine_) == line;
        if (hasBreakpoint) {
            int dot = 5 * pixel_;
            canvas.fill({frame_.x + 2 * pixel_, top + (lineHeight_ - dot) / 2, dot, dot}, bitmap::kLightRed);
        }
        if (isExecution) {
            // A yellow arrow, over the breakpoint's dot if there is one.
            int middle = top + lineHeight_ / 2;
            int arrow = std::max(3, ui(6));
            for (int step = 0; step <= arrow; ++step) {
                int half = arrow - step;
                canvas.fill({frame_.x + ui(2) + step, middle - half, 1, 2 * half + 1}, bitmap::kYellow);
            }
        }
        int severity = diagnosticSeverityForLine(line);
        if (!hasBreakpoint && !isExecution && severity > 0 && severity <= 2) {
            int dot = ui(6);
            canvas.fill({frame_.x + ui(10), top + (lineHeight_ - dot) / 2, dot, dot}, colorForSeverity(severity));
        }
        // Fold arrows: always for folded blocks, for the rest while the pointer is over the numbers.
        bool folded = folds_.isFoldHeader(line);
        if (folded || (gutterHover_ && isLineFoldable(line))) {
            ui::Icon icon = folded ? ui::Icon::Right : ui::Icon::Down;
            ui::Rect size = ui::iconSize(icon, chromePixel);
            ui::drawIcon(canvas, icon, frame_.x + width - ui(18), top + (lineHeight_ - size.h) / 2, chromePixel,
                         folded ? activeLineNumberColor_ : lineNumberColor_, false);
        }
    }
    // Version control: a bar for added and modified lines, a mark where lines were removed.
    int barX = frame_.x + width - ui(6);
    for (const LineChange& change : lineChanges_) {
        int top = frame_.y + static_cast<int>(std::lround(yForLine(change.firstLine) - scrollY_));
        int bottom = frame_.y + static_cast<int>(std::lround(yForLine(change.firstLine + change.lineCount) - scrollY_));
        if (change.kind == LineChangeKind::Removed) {
            if (top < frame_.y - lineHeight_ || top > frame_.bottom() + lineHeight_) {
                continue;
            }
            int size = ui(4);
            for (int step = 0; step <= size; ++step) {
                canvas.fill({barX - 1 + step, top - (size - step), 1, 2 * (size - step) + 1}, 0xF5666BFF);
            }
            continue;
        }
        if (bottom < frame_.y || top > frame_.bottom()) {
            continue;
        }
        Color color = change.kind == LineChangeKind::Added ? 0x57C773FF : 0x5C9EFFFF;
        canvas.fill({barX, top + 1, ui(3), std::max(1, bottom - top - 2)}, color);
    }
    canvas.popClip();
}

void EditorView::drawSquiggle(ui::Canvas& canvas, int left, int right, int top, Color color) {
    // A dashed line of pixels just under the text.
    int y = top + baseline_ + pixel_;
    for (int x = left; x < std::max(right, left + charWidth_ * 4 / 5); x += 3 * pixel_) {
        canvas.fill({x, y, 2 * pixel_, pixel_}, color);
    }
}

void EditorView::drawInlineMessage(ui::Canvas& canvas, const std::string& message, int severity, size_t line) {
    std::string text = message.substr(0, message.find('\n'));
    if (text.empty()) {
        return;
    }
    Color color = colorForSeverity(severity);
    LineLayout& layout = layoutForLine(line);
    ui::Rect area = textArea();
    int endX = area.x - static_cast<int>(scrollX_) + textLeft() + layout.xForByte(layout.x.size() - 1);
    int left = endX + charWidth_ * 3;
    int available = area.right() - left - ui(24);
    if (available < ui(60)) {
        return;
    }
    int top = frame_.y + static_cast<int>(std::lround(yForLine(line) - scrollY_));
    int width = std::min(ui::textWidth(*font_, text), available);
    canvas.fill({left - charWidth_ / 2, top, width + charWidth_, lineHeight_}, ui::withOpacity(color, 0.16));
    canvas.pushClip({left, top, width, lineHeight_});
    ui::drawText(canvas, *font_, left, top, text, color, true);
    canvas.popClip();
}

std::optional<std::pair<Position, Position>> EditorView::bracketMatch() {
    if (!document_ || !document_->selection.empty()) {
        return std::nullopt;
    }
    Position caret = document_->selection.caret;
    if (matchVersion_ == document_->version() && matchCaret_ == caret) {
        return match_;
    }
    matchVersion_ = document_->version();
    matchCaret_ = caret;
    match_.reset();
    const auto& buffer = document_->buffer();
    auto codeMask = [&](size_t line) {
        const std::vector<tm::Token>* tokens =
            highlighter_ && line < highlighter_->validLines() ? highlighter_->tokens(buffer, line, 0) : nullptr;
        return codeMaskForLine(line, tokens);
    };
    std::vector<Position> candidates;
    if (caret.column > 0) {
        candidates.push_back({caret.line, caret.column - 1});
    }
    candidates.push_back(caret);
    for (Position at : candidates) {
        std::string_view text = buffer.line(at.line);
        if (at.column >= text.size()) {
            continue;
        }
        char bracket = text[at.column];
        char partner = matchingByte(bracket);
        if (!partner || !codeMask(at.line)[at.column]) {
            continue;
        }
        bool forward = isOpenerByte(bracket);
        int depth = 0;
        long line = static_cast<long>(at.line);
        long limit = 3000;
        long column = static_cast<long>(at.column);
        while (line >= 0 && line < static_cast<long>(buffer.lineCount()) && limit-- > 0) {
            std::string_view scan = buffer.line(static_cast<size_t>(line));
            std::vector<bool> code = codeMask(static_cast<size_t>(line));
            if (forward) {
                for (long index = column; index < static_cast<long>(scan.size()); ++index) {
                    if (!code[static_cast<size_t>(index)]) {
                        continue;
                    }
                    if (scan[static_cast<size_t>(index)] == bracket) {
                        ++depth;
                    } else if (scan[static_cast<size_t>(index)] == partner && --depth == 0) {
                        match_ = std::make_pair(at, Position{static_cast<size_t>(line), static_cast<size_t>(index)});
                        return match_;
                    }
                }
                ++line;
                column = 0;
            } else {
                for (long index = column; index >= 0; --index) {
                    if (index >= static_cast<long>(scan.size()) || !code[static_cast<size_t>(index)]) {
                        continue;
                    }
                    if (scan[static_cast<size_t>(index)] == bracket) {
                        ++depth;
                    } else if (scan[static_cast<size_t>(index)] == partner && --depth == 0) {
                        match_ = std::make_pair(Position{static_cast<size_t>(line), static_cast<size_t>(index)}, at);
                        return match_;
                    }
                }
                --line;
                column = line >= 0 ? static_cast<long>(buffer.line(static_cast<size_t>(line)).size()) - 1 : 0;
            }
        }
    }
    return std::nullopt;
}

void EditorView::drawText(ui::Canvas& canvas, size_t first, size_t last, bool focused) {
    ui::Rect area = textArea();
    canvas.pushClip(area);
    const auto& buffer = document_->buffer();
    const EditorSettings& settings = Services::shared().settings();
    int originX = area.x - static_cast<int>(std::lround(scrollX_));
    int originY = frame_.y - static_cast<int>(std::lround(scrollY_));
    auto screenRect = [&](double left, double right, double top, int inset) {
        return ui::Rect{originX + static_cast<int>(std::lround(left)), originY + static_cast<int>(std::lround(top)) + inset,
                        std::max(0, static_cast<int>(std::lround(right - left))), lineHeight_ - 2 * inset};
    };
    Range selection = document_->selection.range();
    Position caretPosition = document_->selection.caret;
    std::vector<Range> selectionRanges;
    if (!selection.empty()) {
        selectionRanges.push_back(selection);
    }
    for (const Selection& extra : document_->extraSelections) {
        if (!extra.empty()) {
            selectionRanges.push_back(extra.range());
        }
    }
    bool hasExtraCursors = !document_->extraSelections.empty();

    // The caret's row: a band across the editor.
    if (selection.empty() && !hasExtraCursors && caretPosition.line >= first && caretPosition.line <= last) {
        canvas.fill({area.x, originY + static_cast<int>(std::lround(yForPosition(caretPosition))), area.w, lineHeight_}, lineHighlight_);
    }
    if (executionLine_ >= 0 && static_cast<size_t>(executionLine_) >= first && static_cast<size_t>(executionLine_) <= last &&
        !isLineHidden(static_cast<size_t>(executionLine_))) {
        int top = originY + static_cast<int>(std::lround(yForLine(static_cast<size_t>(executionLine_))));
        canvas.fill({area.x, top, area.w, static_cast<int>(rowsForLine(static_cast<size_t>(executionLine_))) * lineHeight_},
                    ui::withOpacity(bitmap::kYellow, 0.16));
    }
    // Whole-line tints (diffs).
    for (size_t line = first; line <= last && line < lineTints_.size(); ++line) {
        if (lineTints_[line] != 0 && !isLineHidden(line)) {
            canvas.fill({area.x, originY + static_cast<int>(std::lround(yForLine(line))), area.w,
                         static_cast<int>(rowsForLine(line)) * lineHeight_},
                        lineTints_[line]);
        }
    }
    // Flashes after a reload.
    double time = now();
    for (const Flash& flash : flashes_) {
        double age = time - flash.start;
        if (age >= 2 || flash.first > last || flash.first + flash.count <= first) {
            continue;
        }
        double opacity = 0.55 * (1 - age / 2) * (1 - age / 2);
        int top = originY + static_cast<int>(std::lround(yForLine(flash.first)));
        int bottom = originY + static_cast<int>(std::lround(yForLine(flash.first + flash.count)));
        canvas.fill({area.x, top, area.w, bottom - top}, ui::withOpacity(caretColor_, opacity));
    }
    // Find results.
    if (!findMatches_.empty()) {
        auto begin = std::lower_bound(findMatches_.begin(), findMatches_.end(), first,
                                      [](const Range& range, size_t line) { return range.end.line < line; });
        for (auto match = begin; match != findMatches_.end() && match->start.line <= last; ++match) {
            if (match->start.line != match->end.line || isLineHidden(match->start.line)) {
                continue;
            }
            bool isCurrent = static_cast<long>(match - findMatches_.begin()) == currentFindMatch_;
            forEachSegment(match->start.line, match->start.column, match->end.column, false, [&](double left, double right, double top) {
                canvas.fill(screenRect(left, std::max(right, left + 3), top, 0), isCurrent ? currentFindColor_ : findColor_);
            });
        }
    }
    // Backgrounds from color rules and extension decorations.
    for (size_t line = first; line <= last; ++line) {
        if (isLineHidden(line)) {
            continue;
        }
        LineLayout& layout = layoutForLine(line);
        for (const Highlight& highlight : layout.highlights) {
            forEachSegment(line, highlight.start, highlight.end, false, [&](double left, double right, double top) {
                canvas.fill(screenRect(left, right, top, 0), highlight.color);
            });
        }
        auto decorated = decorationsByLine_.find(line);
        if (decorated == decorationsByLine_.end()) {
            continue;
        }
        size_t size = buffer.line(line).size();
        for (const LineDecoration& entry : decorated->second) {
            if (entry.look.background && entry.look.wholeLine) {
                canvas.fill({area.x, originY + static_cast<int>(std::lround(yForLine(line))), area.w,
                             static_cast<int>(rowsForLine(line)) * lineHeight_},
                            *entry.look.background);
                continue;
            }
            if (!entry.look.background && !entry.look.border) {
                continue;
            }
            size_t start = std::min(entry.startByte, size);
            size_t end = entry.toEndOfLine ? size : std::min(entry.endByte, size);
            if (end <= start) {
                continue;
            }
            forEachSegment(line, start, end, false, [&](double left, double right, double top) {
                ui::Rect box = screenRect(left, right, top, 0);
                if (entry.look.background) {
                    canvas.fill(box, *entry.look.background);
                }
                if (entry.look.border) {
                    canvas.frame(box, *entry.look.border, std::max(1, pixel_ / 2));
                }
            });
        }
    }
    // Every whole-word use of the name under the caret.
    if (settings.highlightOccurrences && selection.empty()) {
        std::string word = wordAtCaret();
        if (word.size() >= 2) {
            for (size_t line = first; line <= last; ++line) {
                if (isLineHidden(line)) {
                    continue;
                }
                std::string_view text = buffer.line(line);
                size_t found = text.find(word);
                while (found != std::string::npos) {
                    size_t end = found + word.size();
                    bool startsWord = found == 0 || !isWordByte(static_cast<unsigned char>(text[found - 1]));
                    bool endsWord = end >= text.size() || !isWordByte(static_cast<unsigned char>(text[end]));
                    if (startsWord && endsWord) {
                        forEachSegment(line, found, end, false, [&](double left, double right, double top) {
                            canvas.fill(screenRect(left, right, top, 0), occurrenceColor_);
                        });
                    }
                    found = text.find(word, found + 1);
                }
            }
        }
    }
    // Matching brackets: a solid block behind each, the way the old editors did it.
    if (auto match = bracketMatch()) {
        for (Position at : {match->first, match->second}) {
            if (at.line < first || at.line > last || isLineHidden(at.line)) {
                continue;
            }
            forEachSegment(at.line, at.column, at.column + 1, false, [&](double left, double right, double top) {
                canvas.fill(screenRect(left, right, top, 0), ui::withOpacity(matchColor_, 0.45));
            });
        }
    }

    // The text, with selections behind it.
    std::vector<uint32_t> noUnits;
    for (size_t line = first; line <= last; ++line) {
        if (isLineHidden(line)) {
            continue;
        }
        double lineTop = yForLine(line);
        for (const Range& covered : selectionRanges) {
            if (line < covered.start.line || line > covered.end.line) {
                continue;
            }
            size_t startByte = line == covered.start.line ? covered.start.column : 0;
            size_t endByte = line == covered.end.line ? covered.end.column : buffer.line(line).size();
            bool continues = line < covered.end.line;
            forEachSegment(line, startByte, endByte, continues, [&](double left, double right, double top) {
                canvas.fill(screenRect(left, right, top, 1), selectionColor_);
            });
        }
        LineLayout& layout = layoutForLine(line);
        std::string_view text = buffer.line(line);
        for (size_t byte = 0; byte < text.size();) {
            size_t next = byte;
            char32_t character = ui::decodeUtf8(text, next);
            size_t row = layout.rowForByte(byte);
            int x = originX + textLeft() + layout.x[byte];
            int y = originY + static_cast<int>(std::lround(lineTop)) + static_cast<int>(row) * lineHeight_;
            if (x + 2 * charWidth_ >= area.x && x < area.right()) {
                if (character != '\t' && character != ' ') {
                    ui::drawGlyph(canvas, *font_, character, x, y, layout.colors[byte], true);
                }
                if (layout.underline[byte] == 1) {
                    canvas.fill({x, y + baseline_ + pixel_, charWidth_ * ui::Font::cells(character), pixel_}, layout.colors[byte]);
                } else if (layout.underline[byte] == 2) {
                    canvas.fill({x, y + baseline_ - lineHeight_ / 4, charWidth_ * ui::Font::cells(character), pixel_},
                                layout.colors[byte]);
                }
            }
            byte = next;
        }
        // Inlay hints, as dim chips in the gaps.
        for (const Chip& chip : layout.chips) {
            int x = originX + textLeft() + chip.x;
            int y = originY + static_cast<int>(std::lround(lineTop));
            canvas.fill({x, y + pixel_, chip.width - charWidth_ / 2, lineHeight_ - 2 * pixel_}, ui::withOpacity(dimColor_, 0.14));
            ui::drawText(canvas, *font_, x + charWidth_ / 4, y, chip.label, dimColor_, false);
        }
        // A folded block's "…" marker.
        if (folds_.isFoldHeader(line)) {
            size_t lastRow = layout.rowCount() - 1;
            int x = originX + textLeft() + layout.xForByte(text.size()) + charWidth_;
            int y = originY + static_cast<int>(std::lround(lineTop)) + static_cast<int>(lastRow) * lineHeight_;
            canvas.fill({x, y + 2 * pixel_, charWidth_ * 3, lineHeight_ - 4 * pixel_}, ui::withOpacity(foreground_, 0.14));
            ui::drawGlyph(canvas, *font_, U'…', x + charWidth_, y, ui::withOpacity(foreground_, 0.7), false);
        }
    }

    // Problems: dashed underlines, and the message after the line.
    if (!diagnostics_.empty()) {
        auto begin = std::lower_bound(diagnostics_.begin(), diagnostics_.end(), first,
                                      [](const Diagnostic& item, size_t value) { return item.range.start.line < value; });
        bool showInline = settings.inlineDiagnostics && !wrapEnabled_;
        size_t lastInlineLine = SIZE_MAX;
        for (auto item = begin; item != diagnostics_.end() && item->range.start.line <= last; ++item) {
            size_t line = item->range.start.line;
            if (line >= buffer.lineCount() || isLineHidden(line)) {
                continue;
            }
            Color color = colorForSeverity(item->severity);
            std::string_view text = buffer.line(line);
            size_t startByte = std::min(item->range.start.column, text.size());
            size_t endByte = item->range.end.line == line ? std::min(item->range.end.column, text.size()) : text.size();
            if (endByte <= startByte) {
                Range word = buffer.wordAt({line, startByte});
                if (word.start.line == line && word.end.column > word.start.column) {
                    startByte = word.start.column;
                    endByte = word.end.column;
                } else {
                    endByte = std::min(text.size(), buffer.nextCharacter({line, startByte}).column);
                }
            }
            forEachSegment(line, startByte, endByte, false, [&](double left, double right, double top) {
                ui::Rect box = screenRect(left, right, top, 0);
                drawSquiggle(canvas, box.x, box.right(), box.y, color);
            });
            if (showInline && line != lastInlineLine && rowsForLine(line) == 1 && !folds_.isFoldHeader(line)) {
                lastInlineLine = line;
                drawInlineMessage(canvas, item->message, item->severity, line);
            }
        }
    }
    // Notes extensions put after a line's end.
    if (!decorationsByLine_.empty() && !wrapEnabled_) {
        for (size_t line = first; line <= last; ++line) {
            auto found = decorationsByLine_.find(line);
            if (found == decorationsByLine_.end() || isLineHidden(line) || diagnosticSeverityForLine(line) > 0) {
                continue;
            }
            for (const LineDecoration& entry : found->second) {
                if (!entry.showsNote || entry.look.afterText.empty()) {
                    continue;
                }
                LineLayout& layout = layoutForLine(line);
                int left = originX + textLeft() + layout.xForByte(layout.x.size() - 1) + charWidth_ * 3;
                int top = originY + static_cast<int>(std::lround(yForLine(line)));
                std::string note = entry.look.afterText.substr(0, entry.look.afterText.find('\n'));
                ui::drawText(canvas, *font_, left, top, note, entry.look.afterColor.value_or(dimColor_), false);
                break;
            }
        }
    }
    // The other cursors are plain bars (only the main one blinks).
    for (const Selection& extra : document_->extraSelections) {
        Position at = buffer.clamp(extra.caret);
        if (at.line < first || at.line > last || isLineHidden(at.line)) {
            continue;
        }
        int x = originX + static_cast<int>(std::lround(xForPosition(at)));
        int y = originY + static_cast<int>(std::lround(yForPosition(at)));
        canvas.fill({x, y + baseline_ + pixel_, charWidth_, 2 * pixel_}, ui::withOpacity(caretColor_, 0.9));
    }
    drawCaret(canvas, focused);
    canvas.popClip();
}

// ---------------------------------------------------------------------------
// Caret and the Vim cursor's smear

bool EditorView::vimBlockCursor() const {
    return Services::shared().settings().vimMode && vim_.mode() != VimMode::Insert;
}

ui::Rect EditorView::caretRect() {
    if (vimBlockCursor()) {
        Position cursor = vim_.cursor(*document_);
        return {static_cast<int>(std::lround(xForPosition(cursor))), static_cast<int>(std::lround(yForPosition(cursor))) + 2 * pixel_ / 2,
                charWidth_, lineHeight_ - 2 * (2 * pixel_ / 2)};
    }
    Position caret = document_->selection.caret;
    return {static_cast<int>(std::lround(xForPosition(caret))) - 1, static_cast<int>(std::lround(yForPosition(caret))) + 3, 2,
            lineHeight_ - 6};
}

ui::Rect EditorView::caretLayerRect() {
    if (vimBlockCursor()) {
        return caretRect();
    }
    // While typing: an underscore under the next character, like a DOS text cursor.
    Position caret = document_->selection.caret;
    return {static_cast<int>(std::lround(xForPosition(caret))), static_cast<int>(std::lround(yForPosition(caret))) + baseline_ + pixel_,
            charWidth_, 2 * pixel_};
}

void EditorView::stopSmear() {
    smearing_ = false;
    smear_.stop();
}

void EditorView::updateCaret(bool animated) {
    blinkStart_ = now();
    bool show = document_ != nullptr && focused_;
    if (!show) {
        stopSmear();
        hasLastCaretRect_ = false;
        return;
    }
    ui::Rect target = caretLayerRect();
    bool block = vimBlockCursor();
    if (hasLastCaretRect_ && block != lastCaretWasBlock_) {
        // Going into or out of insert mode: where the caret was, in the new shape.
        lastCaretRect_ = {lastCaretRect_.x, lastCaretRect_.y + lastCaretRect_.h / 2 - target.h / 2, target.w, target.h};
    }
    bool wantsSmear = animated && Services::shared().settings().vimMode && Services::shared().settings().vimCursorSmear &&
                      hasLastCaretRect_ && !(lastCaretRect_ == target);
    if (wantsSmear && block &&
        isRealMove({double(lastCaretRect_.x), double(lastCaretRect_.y), double(lastCaretRect_.w), double(lastCaretRect_.h)},
                   {double(target.x), double(target.y), double(target.w), double(target.h)}, charWidth_, lineHeight_)) {
        smear_.setCharWidth(charWidth_);
        if (!smear_.active()) {
            smear_.jump({double(lastCaretRect_.x), double(lastCaretRect_.y), double(lastCaretRect_.w), double(lastCaretRect_.h)});
        }
        double time = now();
        smear_.moveTo({double(target.x), double(target.y), double(target.w), double(target.h)}, time);
        if (!smearing_) {
            smearing_ = true;
            lastSmearFrame_ = time;
        }
    } else if (!animated || !wantsSmear) {
        stopSmear();
    }
    lastCaretRect_ = target;
    hasLastCaretRect_ = true;
    lastCaretWasBlock_ = block;
}

void EditorView::drawCaret(ui::Canvas& canvas, bool focused) {
    if (!focused || !document_) {
        return;
    }
    ui::Rect area = textArea();
    int originX = area.x - static_cast<int>(std::lround(scrollX_));
    int originY = frame_.y - static_cast<int>(std::lround(scrollY_));
    bool block = vimBlockCursor();
    if (smearing_) {
        // The smear stands in for the caret: a convex shape faded along its length.
        SmearShape shape = smear_.shape();
        if (!shape.outline.empty()) {
            std::vector<ui::Point> points;
            for (const SmearPoint& corner : shape.outline) {
                points.push_back({corner.x + originX, corner.y + originY});
            }
            double dx = shape.fadeEnd.x - shape.fadeStart.x, dy = shape.fadeEnd.y - shape.fadeStart.y;
            double length2 = std::max(1e-6, dx * dx + dy * dy);
            double startX = shape.fadeStart.x + originX, startY = shape.fadeStart.y + originY;
            Color caret = caretColor_;
            const std::vector<SmearFade>& fades = shape.fades;
            canvas.fillConvex(points, [&](double x, double y) -> Color {
                double t = std::clamp(((x - startX) * dx + (y - startY) * dy) / length2, 0.0, 1.0);
                double opacity = fades.empty() ? 1 : fades.back().opacity;
                for (size_t index = 1; index < fades.size(); ++index) {
                    if (t <= fades[index].at) {
                        double span = std::max(1e-6, fades[index].at - fades[index - 1].at);
                        double share = (t - fades[index - 1].at) / span;
                        opacity = fades[index - 1].opacity + (fades[index].opacity - fades[index - 1].opacity) * share;
                        break;
                    }
                }
                return ui::withOpacity(caret, opacity * 0.55);
            });
        }
        return;
    }
    // A hard blink, twice a second.
    double phase = std::fmod(now() - blinkStart_, 2 * kBlinkPeriod);
    if (phase >= kBlinkPeriod) {
        return;
    }
    ui::Rect caret = caretLayerRect().offset(originX, originY);
    canvas.fill(caret, block ? ui::withOpacity(caretColor_, 0.55) : caretColor_);
}

double EditorView::nextFrameTime() const {
    if (!document_ || !focused_) {
        double flashEnd = 0;
        for (const Flash& flash : flashes_) {
            flashEnd = std::max(flashEnd, flash.start + 2);
        }
        return flashEnd > now() ? now() + 1.0 / 30 : 0;
    }
    if (smearing_) {
        return lastSmearFrame_ + 1.0 / 60;
    }
    for (const Flash& flash : flashes_) {
        if (now() - flash.start < 2) {
            return now() + 1.0 / 30;
        }
    }
    double elapsed = now() - blinkStart_;
    double next = std::floor(elapsed / kBlinkPeriod + 1) * kBlinkPeriod;
    return blinkStart_ + next;
}

bool EditorView::tick(double time) {
    bool changed = false;
    if (smearing_) {
        double elapsed = time - lastSmearFrame_;
        lastSmearFrame_ = time;
        if (!smear_.advance(elapsed)) {
            stopSmear();
        }
        changed = true;
    }
    flashes_.erase(std::remove_if(flashes_.begin(), flashes_.end(), [&](const Flash& flash) { return time - flash.start >= 2; }),
                   flashes_.end());
    return changed || focused_ || !flashes_.empty();
}

void EditorView::focusChanged(bool focused) {
    focused_ = focused;
    if (!focused) {
        cancelHover();
    }
    updateCaret(false);
    needsDisplay();
}

// ---------------------------------------------------------------------------
// Selection and scrolling

void EditorView::scrollCaretToVisible() {
    if (!document_) {
        return;
    }
    double x = xForPosition(document_->selection.caret);
    double y = yForPosition(document_->selection.caret);
    double marginX = charWidth_ * 4, marginY = lineHeight_ * 2;
    double newX = scrollX_, newY = scrollY_;
    if (y - marginY < scrollY_) {
        newY = y - marginY;
    } else if (y + lineHeight_ + marginY > scrollY_ + frame_.h) {
        newY = y + lineHeight_ + marginY - frame_.h;
    }
    if (x - marginX < scrollX_) {
        newX = x - marginX;
    } else if (x + charWidth_ + marginX > scrollX_ + visibleWidth()) {
        newX = x + charWidth_ + marginX - visibleWidth();
    }
    scrollX_ = newX;
    scrollY_ = newY;
    clampScroll();
}

void EditorView::scrollToLineCentered(size_t line) {
    scrollY_ = yForLine(line) - frame_.h / 2.0;
    clampScroll();
    needsDisplay();
}

void EditorView::selectionDidChange() {
    if (multiDepth_ > 0) {
        multiMoved_ = true;
        return;
    }
    cancelHover();
    if (document_ && !folds_.empty() && folds_.unfoldHiding(document_->selection.caret.line)) {
        rowLayoutChanged();
    }
    scrollCaretToVisible();
    updateCaret(true);
    needsDisplay();
    if (onSelectionChange) {
        onSelectionChange();
    }
}

void EditorView::didEdit() {
    if (multiDepth_ > 0) {
        multiEdited_ = true;
        return;
    }
    layouts_.clear();
    const auto& buffer = document_->buffer();
    size_t caretLine = document_->selection.caret.line;
    longestLine_ = std::max(longestLine_, buffer.line(std::min(caretLine, buffer.lineCount() - 1)).size());
    if (sectionsTimer_ != 0) {
        cancelTimer(sectionsTimer_);
    }
    sectionsTimer_ = afterDelay(0.3, [this] {
        sectionsTimer_ = 0;
        refreshSections();
    });
    std::optional<std::string> typed = pendingTyped_;
    pendingTyped_.reset();
    selectionDidChange();
    if (onEdit) {
        onEdit();
    }
    if (onTypedText) {
        onTypedText(typed);
    }
}

void EditorView::moveCaretTo(Position position, bool extend) {
    if (!document_) {
        return;
    }
    if (multiDepth_ == 0) {
        document_->extraSelections.clear();
    }
    position = document_->buffer().clamp(position);
    if (extend) {
        document_->selection.caret = position;
    } else {
        document_->selection = {position, position};
    }
    selectionDidChange();
}

void EditorView::selectRange(Range range) {
    if (!document_) {
        return;
    }
    range = range.normalized();
    range.start = document_->buffer().clamp(range.start);
    range.end = document_->buffer().clamp(range.end);
    document_->extraSelections.clear();
    document_->selection = {range.start, range.end};
    document_->preferredX.reset();
    if (!folds_.empty() && folds_.unfoldHiding(range.start.line)) {
        rowLayoutChanged();
    }
    double top = yForLine(range.start.line);
    bool onScreen = top >= scrollY_ + lineHeight_ && top <= scrollY_ + frame_.h - 2 * lineHeight_;
    if (!onScreen) {
        scrollToLineCentered(range.start.line);
    }
    selectionDidChange();
}

void EditorView::jumpToLine(size_t line) {
    jumpToPosition({line, 0});
}

void EditorView::jumpToPosition(Position position) {
    if (!document_) {
        return;
    }
    Position target = document_->buffer().clamp(position);
    document_->extraSelections.clear();
    document_->selection = {target, target};
    scrollToLineCentered(target.line);
    selectionDidChange();
}

void EditorView::flashLines(size_t first, size_t count) {
    if (!document_ || count == 0) {
        return;
    }
    flashes_.push_back({first, count, now()});
    needsDisplay();
}

size_t EditorView::cursorCount() const {
    return document_ ? 1 + document_->extraSelections.size() : 0;
}

void EditorView::editDocument(const std::function<void(Document&)>& edits) {
    if (!document_ || document_->readOnly) {
        return;
    }
    document_->beginUndoGroup();
    edits(*document_);
    document_->endUndoGroup();
    invalidateLayout();
    refreshSections();
    selectionDidChange();
    if (onEdit) {
        onEdit();
    }
}

void EditorView::replaceRange(Range range, const std::string& text) {
    if (!document_) {
        return;
    }
    Position caret = document_->replace(range, text);
    document_->selection = {caret, caret};
    didEdit();
}

// ---------------------------------------------------------------------------
// Queries

std::string EditorView::wordAtCaret() const {
    if (!document_) {
        return std::string();
    }
    const auto& buffer = document_->buffer();
    Range range = buffer.wordAt(document_->selection.caret);
    std::string word = buffer.text(range);
    if (word.empty() || std::isdigit(static_cast<unsigned char>(word[0]))) {
        return std::string();
    }
    for (char character : word) {
        if (!isWordByte(static_cast<unsigned char>(character))) {
            return std::string();
        }
    }
    return word;
}

std::string EditorView::selectedText() const {
    if (!document_) {
        return std::string();
    }
    if (!document_->selection.empty()) {
        return document_->buffer().text(document_->selection.range());
    }
    return wordAtCaret();
}

ui::Rect EditorView::rectForPosition(Position position) {
    if (!document_) {
        return {};
    }
    position = document_->buffer().clamp(position);
    ui::Rect area = textArea();
    int x = area.x - static_cast<int>(std::lround(scrollX_)) + static_cast<int>(std::lround(xForPosition(position)));
    int y = frame_.y - static_cast<int>(std::lround(scrollY_)) + static_cast<int>(std::lround(yForPosition(position)));
    return {x, y, charWidth_, lineHeight_};
}

std::vector<std::string> EditorView::scopesAtCaret() {
    std::vector<std::string> scopes;
    if (!document_ || !highlighter_ || !highlighter_->grammar()) {
        return scopes;
    }
    Position caret = document_->selection.caret;
    const std::vector<tm::Token>* tokens = highlighter_->tokens(document_->buffer(), caret.line);
    if (!tokens || tokens->empty()) {
        return scopes;
    }
    const tm::Token* under = &tokens->back();
    for (const tm::Token& token : *tokens) {
        if (caret.column >= token.start && caret.column < token.end) {
            under = &token;
            break;
        }
    }
    for (const tm::ScopeNode* node = under->scopes; node != nullptr; node = node->parent) {
        scopes.push_back(node->name);
    }
    return scopes;
}

// ---------------------------------------------------------------------------
// Marks

void EditorView::setLineChanges(std::vector<LineChange> changes) {
    lineChanges_ = std::move(changes);
    needsDisplay();
}

void EditorView::setLineTints(std::vector<Color> tints) {
    lineTints_ = std::move(tints);
    needsDisplay();
}

void EditorView::setInlayHints(std::vector<InlayHint> hints) {
    inlayByLine_.clear();
    if (Services::shared().settings().inlayHints) {
        for (InlayHint& hint : hints) {
            inlayByLine_[hint.position.line].push_back(std::move(hint));
        }
    }
    layouts_.clear();
    needsDisplay();
}

void EditorView::setDecorationGroups(const std::map<std::string, std::vector<Decoration>>& groups) {
    decorationsByLine_.clear();
    constexpr size_t kMostLinesPerDecoration = 5000;
    for (const auto& [key, decorations] : groups) {
        for (const Decoration& decoration : decorations) {
            Range range = decoration.range.normalized();
            size_t lastLine = std::min(range.end.line, range.start.line + kMostLinesPerDecoration);
            for (size_t line = range.start.line; line <= lastLine; ++line) {
                LineDecoration entry;
                entry.look = decoration;
                entry.startByte = line == range.start.line ? range.start.column : 0;
                entry.toEndOfLine = line < range.end.line;
                entry.endByte = line == range.end.line ? range.end.column : 0;
                entry.showsNote = line == lastLine;
                decorationsByLine_[line].push_back(std::move(entry));
            }
        }
    }
    layouts_.clear();
    needsDisplay();
}

void EditorView::setDiagnostics(std::vector<Diagnostic> diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.range.start.line != b.range.start.line) {
            return a.range.start.line < b.range.start.line;
        }
        return a.severity < b.severity;
    });
    diagnostics_ = std::move(diagnostics);
    needsDisplay();
}

void EditorView::setSemanticTokens(std::shared_ptr<const SemanticTokenSet> tokens) {
    semantic_ = tokens ? *tokens : SemanticTokenSet();
    semanticStyles_.clear();
    layouts_.clear();
    needsDisplay();
}

void EditorView::setFindMatches(std::vector<Range> matches, long current) {
    findMatches_ = std::move(matches);
    currentFindMatch_ = current;
    needsDisplay();
}

void EditorView::toggleBreakpointAtLine(size_t line) {
    if (!document_ || line >= document_->buffer().lineCount()) {
        return;
    }
    if (document_->breakpointLines.count(line) > 0) {
        document_->breakpointLines.erase(line);
    } else {
        document_->breakpointLines.insert(line);
    }
    needsDisplay();
    if (onBreakpointsChange) {
        onBreakpointsChange();
    }
}

void EditorView::setExecutionLine(long line) {
    if (line != executionLine_) {
        executionLine_ = line;
        needsDisplay();
    }
}

// ---------------------------------------------------------------------------
// Hover

void EditorView::cancelHover() {
    if (hoverTimer_ != 0) {
        cancelTimer(hoverTimer_);
        hoverTimer_ = 0;
    }
    if (hoverActive_) {
        hoverActive_ = false;
        if (onHoverEnd) {
            onHoverEnd();
        }
    }
}

void EditorView::mouseExited() {
    hoverWord_ = Range{{SIZE_MAX, 0}, {SIZE_MAX, 0}};
    cancelHover();
    if (gutterHover_) {
        gutterHover_ = false;
        needsDisplay();
    }
}

void EditorView::mouseMoved(const MouseEvent& event) {
    if (!document_) {
        return;
    }
    bool overGutter = event.x >= frame_.x && event.x < frame_.x + gutterWidth() && frame_.contains(event.x, event.y);
    if (overGutter != gutterHover_) {
        gutterHover_ = overGutter;
        needsDisplay();
    }
    if (!onHoverRequest) {
        return;
    }
    auto [x, y] = documentPoint(event.x, event.y);
    if (!textArea().contains(event.x, event.y) || x < textLeft()) {
        cancelHover();
        hoverWord_ = Range{{SIZE_MAX, 0}, {SIZE_MAX, 0}};
        return;
    }
    Position position = positionForPoint(x, y);
    const auto& buffer = document_->buffer();
    Range word = buffer.wordAt(position);
    std::string_view text = buffer.line(position.line);
    bool onWord = word.start.line == position.line && word.end.column > word.start.column && position.column < text.size() &&
                  !std::isspace(static_cast<unsigned char>(text[position.column]));
    if (onWord) {
        double endX = xForPosition({position.line, text.size()});
        onWord = x <= endX + charWidth_ || rowsForLine(position.line) > 1;
    }
    if (!onWord) {
        cancelHover();
        hoverWord_ = Range{{SIZE_MAX, 0}, {SIZE_MAX, 0}};
        return;
    }
    if (hoverWord_.start == word.start && hoverWord_.end == word.end) {
        return;
    }
    cancelHover();
    hoverWord_ = word;
    hoverTimer_ = afterDelay(0.5, [this] {
        hoverTimer_ = 0;
        if (!document_ || !onHoverRequest || hoverWord_.start.line == SIZE_MAX) {
            return;
        }
        hoverActive_ = true;
        onHoverRequest(hoverWord_.start, hoverWord_);
    });
}

// ---------------------------------------------------------------------------
// Folding

bool EditorView::isLineFoldable(size_t line) const {
    if (!document_ || line >= document_->buffer().lineCount()) {
        return false;
    }
    return isFoldable(document_->buffer(), line, tabSize_);
}

void EditorView::toggleFoldAtLine(size_t line) {
    if (!document_) {
        return;
    }
    if (folds_.isFoldHeader(line)) {
        folds_.unfold(line);
        rowLayoutChanged();
        return;
    }
    if (!isLineFoldable(line)) {
        return;
    }
    folds_.fold(line, foldEnd(document_->buffer(), line, tabSize_));
    moveCaretOutOfHiddenLines();
    rowLayoutChanged();
}

void EditorView::moveCaretOutOfHiddenLines() {
    Selection selection = document_->selection;
    for (const Fold& fold : folds_.all()) {
        bool caretHidden = selection.caret.line > fold.first && selection.caret.line <= fold.last;
        bool anchorHidden = selection.anchor.line > fold.first && selection.anchor.line <= fold.last;
        if (caretHidden || anchorHidden) {
            Position header = {fold.first, document_->buffer().line(fold.first).size()};
            document_->selection = {header, header};
            return;
        }
    }
}

std::optional<size_t> EditorView::foldableLineContaining(size_t line) {
    const auto& buffer = document_->buffer();
    if (isFoldable(buffer, line, tabSize_)) {
        return line;
    }
    size_t limit = line > 5000 ? line - 5000 : 0;
    for (size_t candidate = line; candidate-- > limit;) {
        if (isFoldable(buffer, candidate, tabSize_) && foldEnd(buffer, candidate, tabSize_) >= line) {
            return candidate;
        }
    }
    return std::nullopt;
}

void EditorView::foldCode() {
    if (!document_) {
        return;
    }
    std::optional<size_t> line = foldableLineContaining(document_->selection.caret.line);
    if (!line) {
        beep();
        return;
    }
    if (folds_.isFoldHeader(*line)) {
        std::optional<size_t> outer;
        const auto& buffer = document_->buffer();
        for (size_t candidate = *line; candidate-- > 0 && *line - candidate < 5000;) {
            if (isFoldable(buffer, candidate, tabSize_) && foldEnd(buffer, candidate, tabSize_) >= *line) {
                outer = candidate;
                break;
            }
        }
        if (!outer) {
            return;
        }
        line = outer;
    }
    toggleFoldAtLine(*line);
}

void EditorView::unfoldCode() {
    if (document_ && folds_.unfoldContaining(document_->selection.caret.line)) {
        rowLayoutChanged();
    }
}

void EditorView::foldAllCode() {
    if (!document_) {
        return;
    }
    const auto& buffer = document_->buffer();
    folds_.clear();
    size_t line = 0;
    while (line < buffer.lineCount()) {
        if (isFoldable(buffer, line, tabSize_)) {
            size_t end = foldEnd(buffer, line, tabSize_);
            folds_.fold(line, end);
            line = end + 1;
        } else {
            ++line;
        }
    }
    moveCaretOutOfHiddenLines();
    rowLayoutChanged();
}

void EditorView::unfoldAllCode() {
    if (!folds_.empty()) {
        folds_.clear();
        rowLayoutChanged();
    }
}

// ---------------------------------------------------------------------------
// Mouse

bool EditorView::mouseDown(const MouseEvent& event) {
    if (!document_ || !frame_.contains(event.x, event.y)) {
        return false;
    }
    cancelHover();
    int gutter = gutterWidth();
    bool inGutter = event.x < frame_.x + gutter;
    auto [x, y] = documentPoint(event.x, event.y);
    if (event.button == 3) {
        Position position = positionForPoint(std::max(x, 0.0), y);
        Range selection = document_->selection.range();
        bool insideSelection = !selection.empty() && position >= selection.start && position <= selection.end;
        if (!insideSelection) {
            document_->selection = {position, position};
            selectionDidChange();
        }
        if (onContextMenu) {
            onContextMenu(event.x, event.y);
        }
        return true;
    }
    if (event.button != 1) {
        return true;
    }
    if (inGutter) {
        int local = event.x - frame_.x;
        size_t line = lineAtY(y, nullptr);
        bool inRange = line < document_->buffer().lineCount();
        // The far left of the line numbers: a breakpoint.
        if (local < ui(15) && inRange) {
            toggleBreakpointAtLine(line);
            return true;
        }
        // The fold arrow.
        int arrowLeft = gutter - ui(18) - ui(6);
        int arrowRight = gutter - ui(10);
        if (local >= arrowLeft && local < arrowRight && inRange && (folds_.isFoldHeader(line) || isLineFoldable(line))) {
            toggleFoldAtLine(line);
            return true;
        }
        // A change bar: its details.
        if (onChangeMarkerClick && local >= gutter - ui(10) && !lineChanges_.empty()) {
            for (const LineChange& change : lineChanges_) {
                bool covers = line >= change.firstLine && line < change.firstLine + std::max<size_t>(change.lineCount, 1);
                bool nearRemoval = change.kind == LineChangeKind::Removed && (line == change.firstLine || line + 1 == change.firstLine);
                if ((change.lineCount > 0 && covers) || nearRemoval) {
                    int top = frame_.y + static_cast<int>(std::lround(yForLine(change.firstLine) - scrollY_));
                    int bottom = frame_.y + static_cast<int>(std::lround(yForLine(change.firstLine + change.lineCount) - scrollY_));
                    onChangeMarkerClick(change.firstLine, {frame_.x + gutter - ui(6), top, ui(6), std::max(bottom - top, lineHeight_)});
                    return true;
                }
            }
        }
        x = 0;
    }
    Position position = positionForPoint(x, y);
    document_->preferredX.reset();
    if (event.shift && event.alt && !inGutter) {
        // Shift+Alt-drag: a rectangle of text.
        drag_ = Drag::Column;
        columnStartX_ = x;
        columnStartY_ = y;
        selectColumn(x, y, x, y);
        return true;
    }
    dragAnchor_ = event.shift ? document_->selection.anchor : position;
    dragClicks_ = inGutter ? 3 : event.clicks;
    if (event.alt) {
        // Alt-click: keep what is selected and add another cursor here.
        document_->extraSelections.push_back(document_->selection);
    } else {
        document_->extraSelections.clear();
    }
    drag_ = inGutter ? Drag::Gutter : Drag::Text;
    mouseDragged(event);
    return true;
}

void EditorView::mouseDragged(const MouseEvent& event) {
    if (!document_ || drag_ == Drag::None) {
        return;
    }
    // Past the edges: scroll toward the pointer.
    if (event.y < frame_.y) {
        scrollY_ -= std::min(lineHeight_ * 3, frame_.y - event.y);
    } else if (event.y > frame_.bottom()) {
        scrollY_ += std::min(lineHeight_ * 3, event.y - frame_.bottom());
    }
    clampScroll();
    auto [x, y] = documentPoint(event.x, event.y);
    if (drag_ == Drag::Column) {
        selectColumn(columnStartX_, columnStartY_, x, y);
        return;
    }
    if (drag_ == Drag::Gutter) {
        x = 0;
    }
    const auto& buffer = document_->buffer();
    Position at = positionForPoint(x, std::max(0.0, y));
    Position anchor = dragAnchor_;
    Selection selection;
    if (dragClicks_ >= 3) {
        Position start{std::min(anchor.line, at.line), 0};
        size_t endLine = std::max(anchor.line, at.line);
        bool hasNextLine = endLine + 1 < buffer.lineCount();
        Position end = hasNextLine ? Position{endLine + 1, 0} : Position{endLine, buffer.line(endLine).size()};
        selection = at.line < anchor.line ? Selection{end, start} : Selection{start, end};
    } else if (dragClicks_ == 2) {
        Range firstWord = buffer.wordAt(anchor), lastWord = buffer.wordAt(at);
        selection = at < anchor ? Selection{firstWord.end, lastWord.start} : Selection{firstWord.start, lastWord.end};
    } else {
        selection = {anchor, at};
    }
    document_->selection = selection;
    selectionDidChange();
}

void EditorView::mouseUp(const MouseEvent&) {
    drag_ = Drag::None;
}

void EditorView::selectColumn(double startX, double startY, double endX, double endY) {
    long totalRows = rowMap_.empty() ? static_cast<long>(document_->buffer().lineCount()) : static_cast<long>(rowMap_.totalRows());
    long startRow = std::clamp(static_cast<long>(std::floor(startY / lineHeight_)), 0L, totalRows - 1);
    long endRow = std::clamp(static_cast<long>(std::floor(endY / lineHeight_)), 0L, totalRows - 1);
    long step = endRow >= startRow ? 1 : -1;
    std::vector<Selection> selections;
    size_t previousLine = static_cast<size_t>(-1);
    for (long row = startRow;; row += step) {
        double middle = row * lineHeight_ + lineHeight_ / 2.0;
        Position first = positionForPoint(startX, middle);
        Position second = positionForPoint(endX, middle);
        if (first.line != previousLine) {
            selections.push_back(Selection{first, second});
            previousLine = first.line;
        }
        if (row == endRow) {
            break;
        }
    }
    document_->selection = selections.back();
    document_->extraSelections.assign(selections.begin(), selections.end() - 1);
    normalizeCursors();
    selectionDidChange();
}

bool EditorView::wheel(const WheelEvent& event) {
    if (!document_ || !frame_.contains(event.x, event.y)) {
        return false;
    }
    double dx = event.dx, dy = event.dy;
    if (event.shift && dx == 0) {
        dx = -dy;
        dy = 0;
    }
    scrollTo(scrollX_ + dx * charWidth_ * 4, scrollY_ - dy * lineHeight_ * 3);
    return true;
}

// ---------------------------------------------------------------------------
// Keyboard

std::string EditorView::vimStatus() const {
    if (!Services::shared().settings().vimMode) {
        return std::string();
    }
    std::string mode = vim_.modeName();
    std::string pending = vim_.pending();
    if (vim_.mode() == VimMode::Command) {
        return pending;
    }
    return pending.empty() ? mode : mode + "  " + pending;
}

bool EditorView::insertsText() const {
    return !Services::shared().settings().vimMode || vim_.mode() == VimMode::Insert;
}

void EditorView::vimStatusDidChange() {
    std::string status = vimStatus();
    if (status == vimShownStatus_) {
        return;
    }
    vimShownStatus_ = status;
    if (onVimStatusChange) {
        onVimStatusChange();
    }
}

bool EditorView::handleVimKey(const std::string& key) {
    bool inserting = vim_.mode() == VimMode::Insert;
    if (key == "\x1b" && onHoverEnd) {
        onHoverEnd();  // Escape closes a hint bubble
    }
    if (inserting && key == "\x1b" && onCompletionKey && onCompletionKey("escape")) {
        return true;  // Escape closed the suggestion list first
    }
    vim_.indentUnit = indentUnit();
    uint64_t version = document_->version();
    if (!vim_.handleKey(*document_, key)) {
        return false;
    }
    if (document_->version() != version) {
        didEdit();
    } else {
        selectionDidChange();
    }
    vimStatusDidChange();
    return true;
}

bool EditorView::keyDown(const KeyPress& key) {
    if (!document_) {
        return false;
    }
    bool vimOn = Services::shared().settings().vimMode;
    bool inserting = vim_.mode() == VimMode::Insert;
    // Vim first: Escape, Ctrl-[ and Ctrl-C (normal mode), Ctrl-R (redo), Return and Backspace.
    if (vimOn && !key.meta) {
        std::string vimKey;
        if (key.key == "escape" && !key.control && !key.alt) {
            vimKey = "\x1b";
        } else if (key.control && !key.alt && !key.shift) {
            if (key.key == "[" || (key.key == "c" && !inserting)) {
                vimKey = "\x1b";
            } else if (key.key == "r" && !inserting) {
                vimKey = "\x12";
            }
        } else if (key.plain() && key.key == "enter" && !inserting) {
            vimKey = "\r";
        } else if (key.plain() && key.key == "backspace" && !inserting) {
            vimKey = "\x7f";
        } else if (key.plain() && key.key == "tab" && !inserting) {
            return true;  // Tab means nothing in normal mode
        }
        if (!vimKey.empty() && handleVimKey(vimKey)) {
            return true;
        }
    }
    // The completion list takes the keys it uses.
    if (onCompletionKey && !key.control && !key.alt) {
        static const std::set<std::string> listKeys = {"up", "down", "pageup", "pagedown", "enter", "tab", "escape"};
        if (listKeys.count(key.key) && !key.shift && onCompletionKey(key.key)) {
            return true;
        }
    }
    bool shift = key.shift;
    bool control = key.control;
    if (key.alt || key.meta) {
        return false;
    }
    const std::string& name = key.key;
    if (name == "left") {
        moveHorizontally(-1, control, shift);
    } else if (name == "right") {
        moveHorizontally(1, control, shift);
    } else if (name == "up" && !control) {
        moveVertically(-1, shift);
    } else if (name == "down" && !control) {
        moveVertically(1, shift);
    } else if (name == "up" && control && !shift) {
        scrollTo(scrollX_, scrollY_ - lineHeight_);
    } else if (name == "down" && control && !shift) {
        scrollTo(scrollX_, scrollY_ + lineHeight_);
    } else if (name == "pageup") {
        moveVertically(-pageLines(), shift);
    } else if (name == "pagedown") {
        moveVertically(pageLines(), shift);
    } else if (name == "home") {
        if (control) {
            moveCaretTo({0, 0}, shift);
        } else {
            moveToLineStart(shift);
        }
    } else if (name == "end") {
        if (control) {
            moveCaretTo(document_->buffer().end(), shift);
        } else {
            moveToLineEnd(shift);
        }
    } else if (name == "backspace" && !shift) {
        if (control) {
            deleteWordBackward();
        } else {
            deleteBackward();
        }
    } else if (name == "backspace" && shift) {
        deleteBackward();
    } else if (name == "delete" && !shift) {
        if (control) {
            deleteWordForward();
        } else {
            deleteForward();
        }
    } else if (name == "enter" && !control) {
        if (document_->readOnly) {
            return true;
        }
        insertNewline();
    } else if (name == "tab" && !control) {
        if (document_->readOnly) {
            return true;
        }
        if (shift) {
            insertBacktab();
        } else {
            insertTab();
        }
    } else if (name == "escape" && !control) {
        cancelOperation();
    } else {
        return false;
    }
    return true;
}

void EditorView::textInput(const std::string& text) {
    if (!document_ || text.empty()) {
        return;
    }
    if (Services::shared().settings().vimMode) {
        // Each character goes to Vim; in insert mode it hands them back as typing.
        size_t index = 0;
        while (index < text.size()) {
            size_t start = index;
            ui::decodeUtf8(text, index);
            std::string character = text.substr(start, index - start);
            if (handleVimKey(character)) {
                continue;
            }
            if (vim_.mode() == VimMode::Insert && !document_->readOnly) {
                insertText(character);  // normal mode: keys Vim doesn't know type nothing
            }
        }
        vimStatusDidChange();
        return;
    }
    if (!document_->readOnly) {
        insertText(text);
    }
}

// ---------------------------------------------------------------------------
// Movement

void EditorView::moveHorizontally(int direction, bool byWord, bool extend) {
    FOR_EACH_CURSOR(moveHorizontally(direction, byWord, extend))
    const auto& buffer = document_->buffer();
    Selection selection = document_->selection;
    document_->preferredX.reset();
    if (!extend && !selection.empty() && !byWord) {
        Range range = selection.range();
        moveCaretTo(direction < 0 ? range.start : range.end, false);
        return;
    }
    Position caret = selection.caret;
    Position target;
    if (direction < 0) {
        target = byWord ? buffer.previousWordStart(caret) : buffer.previousCharacter(caret);
    } else {
        target = byWord ? buffer.nextWordEnd(caret) : buffer.nextCharacter(caret);
    }
    moveCaretTo(target, extend);
}

void EditorView::moveVertically(long rows, bool extend) {
    FOR_EACH_CURSOR(moveVertically(rows, extend))
    const auto& buffer = document_->buffer();
    Position caret = document_->selection.caret;
    double x = document_->preferredX.value_or(xForPosition(caret));
    double caretRowTop = yForPosition(caret);
    long caretRow = static_cast<long>(std::floor(caretRowTop / lineHeight_ + 0.5));
    long totalRows = rowMap_.empty() ? static_cast<long>(buffer.lineCount()) : static_cast<long>(rowMap_.totalRows());
    long target = caretRow + rows;
    Position position;
    if (target < 0) {
        position = {0, 0};
    } else if (target >= totalRows) {
        position = buffer.end();
    } else {
        position = positionForPoint(x, target * lineHeight_ + lineHeight_ / 2.0);
    }
    moveCaretTo(position, extend);
    document_->preferredX = x;
}

long EditorView::pageLines() const {
    return std::max(1L, static_cast<long>(frame_.h / std::max(1, lineHeight_)) - 2);
}

void EditorView::moveToLineStart(bool extend) {
    FOR_EACH_CURSOR(moveToLineStart(extend))
    // Smart home: the first non-blank character, then column 0.
    Position caret = document_->selection.caret;
    size_t indent = document_->buffer().indentEnd(caret.line);
    document_->preferredX.reset();
    moveCaretTo({caret.line, caret.column == indent ? 0 : indent}, extend);
}

void EditorView::moveToLineEnd(bool extend) {
    FOR_EACH_CURSOR(moveToLineEnd(extend))
    Position caret = document_->selection.caret;
    document_->preferredX.reset();
    moveCaretTo({caret.line, document_->buffer().line(caret.line).size()}, extend);
}

void EditorView::selectAll() {
    if (!document_) {
        return;
    }
    document_->extraSelections.clear();
    document_->selection = {{0, 0}, document_->buffer().end()};
    selectionDidChange();
}

void EditorView::cancelOperation() {
    if (!document_) {
        return;
    }
    if (onHoverEnd) {
        onHoverEnd();
    }
    document_->endSnippet();
    if (!document_->extraSelections.empty()) {
        // Escape first goes back to one cursor.
        document_->extraSelections.clear();
        needsDisplay();
        updateCaret(false);
        return;
    }
    moveCaretTo(document_->selection.caret, false);
}

// ---------------------------------------------------------------------------
// Typing

std::string EditorView::indentUnit() const {
    return useTabs_ ? std::string("\t") : std::string(tabSize_, ' ');
}

std::string EditorView::textAround(Position position, bool before) const {
    const auto& buffer = document_->buffer();
    if (before) {
        Position previous = buffer.previousCharacter(position);
        return previous.line == position.line ? buffer.text({previous, position}) : std::string();
    }
    Position next = buffer.nextCharacter(position);
    return next.line == position.line ? buffer.text({position, next}) : std::string();
}

void EditorView::insertText(const std::string& text) {
    if (!document_ || text.empty()) {
        return;
    }
    FOR_EACH_CURSOR(insertText(text))
    pendingTyped_ = text;
    Selection selection = document_->selection;
    std::string next = textAround(selection.caret, false);
    std::string previous = textAround(selection.caret, true);
    // Typing a closing character right before the same one steps over it.
    for (const auto& [open, close] : configuration_.autoClosingPairs) {
        if (selection.empty() && text == close && next == close) {
            moveHorizontally(1, false, false);
            return;
        }
    }
    for (const auto& [open, close] : configuration_.autoClosingPairs) {
        if (text != open) {
            continue;
        }
        if (!selection.empty() && selection.range().start.line == selection.range().end.line) {
            // Wrap the selection: (selected) / "selected".
            Range range = selection.range();
            std::string inner = document_->buffer().text(range);
            document_->replace(range, open + inner + close);
            Position start = {range.start.line, range.start.column + open.size()};
            document_->selection = {start, {start.line, start.column + inner.size()}};
            didEdit();
            return;
        }
        bool nextAllowsClose = next.empty() || next == " " || next == "\t" || next == ")" || next == "]" || next == "}" ||
                               next == ";" || next == ",";
        bool isQuote = open == close;
        bool afterWord = !previous.empty() && isWordByte(static_cast<unsigned char>(previous.back()));
        if (selection.empty() && nextAllowsClose && !(isQuote && afterWord)) {
            document_->typeText(open + close);
            Position caret = document_->selection.caret;
            document_->selection.caret = {caret.line, caret.column - close.size()};
            document_->selection.anchor = document_->selection.caret;
            didEdit();
            return;
        }
    }
    document_->typeText(text);
    didEdit();
}

void EditorView::insertNewline() {
    if (!document_) {
        return;
    }
    FOR_EACH_CURSOR(insertNewline())
    Position caret = document_->selection.range().start;
    std::string_view line = document_->buffer().line(caret.line);
    size_t indentEnd = std::min(document_->buffer().indentEnd(caret.line), caret.column);
    std::string indent(line.substr(0, indentEnd));
    std::string previous = textAround(document_->selection.range().start, true);
    std::string next = textAround(document_->selection.range().end, false);
    if (isOpeningBracket(previous)) {
        std::string inner = "\n" + indent + indentUnit();
        bool closesRightAfter = (previous == "{" && next == "}") || (previous == "[" && next == "]") || (previous == "(" && next == ")");
        if (closesRightAfter) {
            // The closing bracket on its own line, the caret on an indented line between.
            document_->replace(document_->selection.range(), inner + "\n" + indent);
            Position end = document_->selection.caret;
            document_->selection = {{end.line - 1, inner.size() - 1}, {end.line - 1, inner.size() - 1}};
        } else {
            document_->replace(document_->selection.range(), inner);
        }
    } else {
        document_->replace(document_->selection.range(), "\n" + indent);
    }
    didEdit();
}

void EditorView::insertTab() {
    if (!document_) {
        return;
    }
    // While a snippet is being filled in, Tab moves to its next placeholder.
    if (multiDepth_ == 0 && document_->jumpSnippetStop(+1)) {
        selectionDidChange();
        return;
    }
    FOR_EACH_CURSOR(insertTab())
    Range range = document_->selection.range();
    if (range.start.line != range.end.line) {
        indentLines();
        return;
    }
    if (useTabs_) {
        document_->typeText("\t");
    } else {
        size_t spaces = tabSize_ - (range.start.column % tabSize_);
        document_->typeText(std::string(spaces, ' '));
    }
    didEdit();
}

void EditorView::insertBacktab() {
    if (document_ && multiDepth_ == 0 && document_->jumpSnippetStop(-1)) {
        selectionDidChange();
        return;
    }
    outdentLines();
}

void EditorView::deleteBackward() {
    if (!document_ || document_->readOnly) {
        return;
    }
    FOR_EACH_CURSOR(deleteBackward())
    const auto& buffer = document_->buffer();
    Selection selection = document_->selection;
    if (!selection.empty()) {
        document_->deleteSelectionOr(selection.range());
        didEdit();
        return;
    }
    Position caret = selection.caret;
    if (caret == Position{0, 0}) {
        return;
    }
    // Backspace between an empty pair removes both halves: (|) -> |
    std::string previous = textAround(caret, true);
    std::string next = textAround(caret, false);
    for (const auto& [open, close] : configuration_.autoClosingPairs) {
        if (previous == open && next == close) {
            document_->replace({buffer.previousCharacter(caret), buffer.nextCharacter(caret)}, "");
            didEdit();
            return;
        }
    }
    // In leading spaces, back to the previous indent stop.
    std::string_view line = buffer.line(caret.line);
    if (!useTabs_ && caret.column > 0 && caret.column <= buffer.indentEnd(caret.line) && line.find_first_not_of(' ', 0) >= caret.column) {
        size_t stop = ((caret.column - 1) / tabSize_) * tabSize_;
        document_->replace({{caret.line, stop}, caret}, "");
        didEdit();
        return;
    }
    document_->replace({buffer.previousCharacter(caret), caret}, "");
    didEdit();
}

void EditorView::deleteForward() {
    if (!document_ || document_->readOnly) {
        return;
    }
    FOR_EACH_CURSOR(deleteForward())
    Position caret = document_->selection.caret;
    document_->deleteSelectionOr({caret, document_->buffer().nextCharacter(caret)});
    didEdit();
}

void EditorView::deleteWordBackward() {
    if (!document_ || document_->readOnly) {
        return;
    }
    FOR_EACH_CURSOR(deleteWordBackward())
    Position caret = document_->selection.caret;
    document_->deleteSelectionOr({document_->buffer().previousWordStart(caret), caret});
    didEdit();
}

void EditorView::deleteWordForward() {
    if (!document_ || document_->readOnly) {
        return;
    }
    FOR_EACH_CURSOR(deleteWordForward())
    Position caret = document_->selection.caret;
    document_->deleteSelectionOr({caret, document_->buffer().nextWordEnd(caret)});
    didEdit();
}

// ---------------------------------------------------------------------------
// Line commands

std::pair<size_t, size_t> EditorView::selectedLines() const {
    Range range = document_->selection.range();
    size_t last = range.end.line;
    if (last > range.start.line && range.end.column == 0) {
        --last;
    }
    return {range.start.line, last};
}

void EditorView::expandSelection() {
    if (!document_) {
        return;
    }
    if (!sameSelection(document_->selection, lastExpanded_)) {
        expandHistory_.clear();
    }
    Range bigger = expandedSelection(document_->buffer(), document_->selection.range());
    Range current = document_->selection.range();
    if (bigger.start == current.start && bigger.end == current.end) {
        beep();
        return;
    }
    expandHistory_.push_back(document_->selection);
    document_->extraSelections.clear();
    document_->selection = {bigger.start, bigger.end};
    lastExpanded_ = document_->selection;
    selectionDidChange();
}

void EditorView::shrinkSelection() {
    if (!document_) {
        return;
    }
    if (expandHistory_.empty() || !sameSelection(document_->selection, lastExpanded_)) {
        expandHistory_.clear();
        beep();
        return;
    }
    document_->extraSelections.clear();
    document_->selection = expandHistory_.back();
    expandHistory_.pop_back();
    lastExpanded_ = document_->selection;
    selectionDidChange();
}

void EditorView::goToMatchingBracket() {
    if (!document_) {
        return;
    }
    auto match = bracketMatch();
    if (!match) {
        beep();
        return;
    }
    Position caret = document_->selection.caret;
    Position opener = match->first, closer = match->second;
    bool touchingOpener = caret == opener || (caret.line == opener.line && caret.column == opener.column + 1);
    moveCaretTo(touchingOpener ? Position{closer.line, closer.column + 1} : opener, false);
}

void EditorView::transformSelectedText(std::string (*transform)(std::string_view), bool wholeLines) {
    if (!document_ || document_->readOnly) {
        return;
    }
    const auto& buffer = document_->buffer();
    Range range = document_->selection.range();
    if (wholeLines) {
        if (document_->selection.empty()) {
            beep();
            return;
        }
        auto [first, last] = selectedLines();
        range = {{first, 0}, {last, buffer.line(last).size()}};
    } else if (document_->selection.empty()) {
        Position caret = document_->selection.caret;
        std::string_view line = buffer.line(caret.line);
        size_t start = std::min(caret.column, line.size());
        size_t end = start;
        while (start > 0 && isWordByte(static_cast<unsigned char>(line[start - 1]))) {
            --start;
        }
        while (end < line.size() && isWordByte(static_cast<unsigned char>(line[end]))) {
            ++end;
        }
        if (start == end) {
            beep();
            return;
        }
        range = {{caret.line, start}, {caret.line, end}};
    }
    std::string original = buffer.text(range);
    std::string replacement = transform(original);
    if (replacement == original) {
        return;
    }
    document_->extraSelections.clear();
    document_->replaceKeepingBreaks(range, replacement);
    document_->selection = {range.start, endOfInsertion(range.start, replacement)};
    didEdit();
}

void EditorView::makeUpperCase() { transformSelectedText(upperCaseText, false); }
void EditorView::makeLowerCase() { transformSelectedText(lowerCaseText, false); }
void EditorView::makeTitleCase() { transformSelectedText(titleCaseText, false); }
void EditorView::sortLinesAscending() { transformSelectedText(sortAscending, true); }
void EditorView::sortLinesDescending() { transformSelectedText(sortDescending, true); }
void EditorView::removeDuplicateLines() { transformSelectedText(uniqueLinesText, true); }

void EditorView::joinSelectedLines() {
    if (!document_ || document_->readOnly) {
        return;
    }
    if (document_->selection.empty()) {
        size_t line = document_->selection.caret.line;
        if (line + 1 >= document_->buffer().lineCount()) {
            beep();
            return;
        }
        document_->selection = {{line, 0}, {line + 1, document_->buffer().line(line + 1).size()}};
    }
    transformSelectedText(joinLinesText, true);
}

void EditorView::transformLines(const std::function<std::string(const std::string&, size_t)>& transform) {
    if (!document_) {
        return;
    }
    auto [first, last] = selectedLines();
    const auto& buffer = document_->buffer();
    Selection before = document_->selection;
    std::string replacement;
    for (size_t line = first; line <= last; ++line) {
        if (line > first) {
            replacement += "\n";
        }
        replacement += transform(std::string(buffer.line(line)), line);
    }
    size_t oldLength = buffer.line(first).size();
    document_->replaceKeepingBreaks({{first, 0}, {last, buffer.line(last).size()}}, replacement);
    const auto& after = document_->buffer();
    if (first == last && before.empty()) {
        long delta = static_cast<long>(after.line(first).size()) - static_cast<long>(oldLength);
        size_t column = static_cast<size_t>(std::max(0L, static_cast<long>(before.caret.column) + delta));
        document_->selection = {{first, column}, {first, column}};
    } else {
        document_->selection = {{first, 0}, {last, after.line(last).size()}};
    }
    didEdit();
}

void EditorView::indentLines() {
    if (!document_ || document_->readOnly) {
        return;
    }
    FOR_EACH_CURSOR_LINE(indentLines())
    std::string unit = indentUnit();
    transformLines([&](const std::string& line, size_t) { return line.empty() ? line : unit + line; });
}

void EditorView::outdentLines() {
    if (!document_ || document_->readOnly) {
        return;
    }
    FOR_EACH_CURSOR_LINE(outdentLines())
    bool tabs = useTabs_;
    size_t tabSize = tabSize_;
    transformLines([&](const std::string& line, size_t) {
        if (tabs && !line.empty() && line[0] == '\t') {
            return line.substr(1);
        }
        size_t spaces = 0;
        while (spaces < tabSize && spaces < line.size() && line[spaces] == ' ') {
            ++spaces;
        }
        return line.substr(spaces);
    });
}

void EditorView::toggleLineComment() {
    if (!document_ || document_->readOnly) {
        return;
    }
    FOR_EACH_CURSOR_LINE(toggleLineComment())
    std::string marker = configuration_.lineComment;
    if (marker.empty()) {
        beep();
        return;
    }
    auto [first, last] = selectedLines();
    const auto& buffer = document_->buffer();
    // Comment everything unless every non-blank line already is.
    bool allCommented = true;
    size_t minIndent = std::string::npos;
    for (size_t line = first; line <= last; ++line) {
        std::string_view text = buffer.line(line);
        size_t indent = buffer.indentEnd(line);
        if (indent == text.size()) {
            continue;
        }
        minIndent = std::min(minIndent, indent);
        if (text.compare(indent, marker.size(), marker) != 0) {
            allCommented = false;
        }
    }
    if (minIndent == std::string::npos) {
        minIndent = 0;
    }
    transformLines([&](const std::string& line, size_t) {
        size_t indent = line.find_first_not_of(" \t");
        if (indent == std::string::npos) {
            return line;
        }
        if (allCommented) {
            size_t length = marker.size();
            if (line.size() > indent + length && line[indent + length] == ' ') {
                ++length;
            }
            return line.substr(0, indent) + line.substr(indent + length);
        }
        return line.substr(0, minIndent) + marker + " " + line.substr(minIndent);
    });
}

std::string EditorView::joinedTextOfLines(size_t first, size_t last) const {
    const auto& buffer = document_->buffer();
    std::string joined;
    for (size_t line = first; line <= last; ++line) {
        if (line > first) {
            joined += "\n";
        }
        joined += buffer.line(line);
    }
    return joined;
}

void EditorView::moveLinesUp() {
    if (!document_ || document_->readOnly) {
        return;
    }
    document_->extraSelections.clear();
    auto [first, last] = selectedLines();
    if (first == 0) {
        return;
    }
    Selection before = document_->selection;
    const auto& buffer = document_->buffer();
    std::string lineAbove(buffer.line(first - 1));
    std::string moved = joinedTextOfLines(first, last);
    document_->replaceKeepingBreaks({{first - 1, 0}, {last, buffer.line(last).size()}}, moved + "\n" + lineAbove);
    document_->selection = {{before.anchor.line - 1, before.anchor.column}, {before.caret.line - 1, before.caret.column}};
    didEdit();
}

void EditorView::moveLinesDown() {
    if (!document_ || document_->readOnly) {
        return;
    }
    document_->extraSelections.clear();
    auto [first, last] = selectedLines();
    if (last + 1 >= document_->buffer().lineCount()) {
        return;
    }
    Selection before = document_->selection;
    const auto& buffer = document_->buffer();
    std::string lineBelow(buffer.line(last + 1));
    std::string moved = joinedTextOfLines(first, last);
    document_->replaceKeepingBreaks({{first, 0}, {last + 1, buffer.line(last + 1).size()}}, lineBelow + "\n" + moved);
    document_->selection = {{before.anchor.line + 1, before.anchor.column}, {before.caret.line + 1, before.caret.column}};
    didEdit();
}

void EditorView::duplicateLinesDown() {
    if (!document_ || document_->readOnly) {
        return;
    }
    document_->extraSelections.clear();
    auto [first, last] = selectedLines();
    Selection before = document_->selection;
    size_t count = last - first + 1;
    std::string block = joinedTextOfLines(first, last);
    Position endOfLast = {last, document_->buffer().line(last).size()};
    document_->replace({endOfLast, endOfLast}, "\n" + block);
    document_->selection = {{before.anchor.line + count, before.anchor.column}, {before.caret.line + count, before.caret.column}};
    didEdit();
}

void EditorView::duplicateLinesUp() {
    if (!document_ || document_->readOnly) {
        return;
    }
    document_->extraSelections.clear();
    auto [first, last] = selectedLines();
    Selection before = document_->selection;
    size_t count = last - first + 1;
    std::string block = joinedTextOfLines(first, last);
    document_->replace({{first, 0}, {first, 0}}, block + "\n");
    document_->selection = {{before.anchor.line + count, before.anchor.column}, {before.caret.line + count, before.caret.column}};
    didEdit();
}

void EditorView::deleteLines() {
    if (!document_ || document_->readOnly) {
        return;
    }
    document_->extraSelections.clear();
    auto [first, last] = selectedLines();
    const auto& buffer = document_->buffer();
    size_t lineCount = buffer.lineCount();
    Range doomed;
    size_t caretLine = first;
    if (last + 1 < lineCount) {
        doomed = {{first, 0}, {last + 1, 0}};
    } else if (first > 0) {
        doomed = {{first - 1, buffer.line(first - 1).size()}, {last, buffer.line(last).size()}};
        caretLine = first - 1;
    } else {
        doomed = {{0, 0}, {last, buffer.line(last).size()}};
    }
    document_->replace(doomed, "");
    document_->selection = {{caretLine, 0}, {caretLine, 0}};
    document_->preferredX.reset();
    didEdit();
}

void EditorView::insertLineBelow() {
    if (!document_ || document_->readOnly) {
        return;
    }
    size_t line = document_->selection.range().end.line;
    Position endOfLine = {line, document_->buffer().line(line).size()};
    document_->selection = {endOfLine, endOfLine};
    insertNewline();
}

void EditorView::insertLineAbove() {
    if (!document_ || document_->readOnly) {
        return;
    }
    size_t line = document_->selection.range().start.line;
    const auto& buffer = document_->buffer();
    std::string indent(buffer.line(line).substr(0, buffer.indentEnd(line)));
    document_->replace({{line, 0}, {line, 0}}, indent + "\n");
    Position caret = {line, indent.size()};
    document_->selection = {caret, caret};
    didEdit();
}

// ---------------------------------------------------------------------------
// Multiple cursors

std::vector<Selection> EditorView::selectionsInOrder() const {
    std::vector<Selection> all;
    all.push_back(document_->selection);
    for (const Selection& extra : document_->extraSelections) {
        all.push_back(extra);
    }
    std::stable_sort(all.begin(), all.end(), [](const Selection& a, const Selection& b) { return a.range().start < b.range().start; });
    return all;
}

void EditorView::normalizeCursors() {
    std::vector<Selection> kept;
    std::vector<Selection> extras = document_->extraSelections;
    std::stable_sort(extras.begin(), extras.end(), [](const Selection& a, const Selection& b) { return a.range().start < b.range().start; });
    for (const Selection& extra : extras) {
        if (selectionsOverlap(extra, document_->selection)) {
            continue;
        }
        bool duplicate = false;
        for (const Selection& existing : kept) {
            if (selectionsOverlap(extra, existing)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            kept.push_back(extra);
        }
    }
    document_->extraSelections = kept;
}

void EditorView::forEachSelection(const std::function<void()>& body, bool mergeLines) {
    Document& document = *document_;
    struct Entry {
        Selection selection;
        bool primary;
    };
    std::vector<Entry> entries;
    entries.push_back({document.selection, true});
    for (const Selection& extra : document.extraSelections) {
        entries.push_back({extra, false});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.selection.range().start < b.selection.range().start; });
    if (mergeLines) {
        std::vector<Entry> merged;
        for (const Entry& entry : entries) {
            if (!merged.empty() && entry.selection.range().start.line <= merged.back().selection.range().end.line) {
                if (entry.primary) {
                    merged.back().primary = true;
                }
                continue;
            }
            merged.push_back(entry);
        }
        entries = merged;
    }
    std::vector<Selection> current;
    size_t primaryIndex = 0;
    for (size_t index = 0; index < entries.size(); ++index) {
        current.push_back(entries[index].selection);
        if (entries[index].primary) {
            primaryIndex = index;
        }
    }
    ++multiDepth_;
    multiEdited_ = false;
    multiMoved_ = false;
    document.beginUndoGroup();
    for (size_t index = entries.size(); index-- > 0;) {
        cursorIndexInLoop_ = index;
        document.selection = current[index];
        document.extraSelections.clear();
        for (size_t other = 0; other < current.size(); ++other) {
            if (other != index) {
                document.extraSelections.push_back(current[other]);
            }
        }
        document.preferredX.reset();
        body();
        current[index] = document.selection;
        if (document.extraSelections.size() == current.size() - 1) {
            size_t next = 0;
            for (size_t other = 0; other < current.size(); ++other) {
                if (other != index) {
                    current[other] = document.extraSelections[next++];
                }
            }
        }
    }
    document.endUndoGroup();
    --multiDepth_;
    document.selection = current[primaryIndex];
    document.extraSelections.clear();
    for (size_t index = 0; index < current.size(); ++index) {
        if (index != primaryIndex) {
            document.extraSelections.push_back(current[index]);
        }
    }
    normalizeCursors();
    if (multiEdited_) {
        didEdit();
    } else if (multiMoved_) {
        selectionDidChange();
    }
    needsDisplay();
}

void EditorView::addCursorInDirection(long direction) {
    if (!document_) {
        return;
    }
    Position caret = document_->selection.caret;
    double x = document_->preferredX.value_or(xForPosition(caret));
    double top = yForPosition(caret);
    long row = static_cast<long>(std::floor(top / lineHeight_ + 0.5)) + direction;
    long totalRows = rowMap_.empty() ? static_cast<long>(document_->buffer().lineCount()) : static_cast<long>(rowMap_.totalRows());
    if (row < 0 || row >= totalRows) {
        beep();
        return;
    }
    Position target = positionForPoint(x, row * lineHeight_ + lineHeight_ / 2.0);
    if (target == caret) {
        return;
    }
    document_->extraSelections.push_back(document_->selection);
    document_->selection = {target, target};
    document_->preferredX = x;
    normalizeCursors();
    selectionDidChange();
}

void EditorView::addCursorAbove() { addCursorInDirection(-1); }
void EditorView::addCursorBelow() { addCursorInDirection(1); }

void EditorView::selectAllOccurrences() {
    if (!document_) {
        return;
    }
    const auto& buffer = document_->buffer();
    Selection selection = document_->selection;
    std::string needle;
    if (selection.empty()) {
        needle = wordAtCaret();
    } else if (selection.range().start.line == selection.range().end.line) {
        needle = buffer.text(selection.range());
    }
    if (needle.empty()) {
        beep();
        return;
    }
    std::vector<Selection> found;
    for (size_t line = 0; line < buffer.lineCount(); ++line) {
        std::string_view text = buffer.line(line);
        size_t at = text.find(needle);
        while (at != std::string::npos) {
            bool wholeWord = true;
            if (selection.empty()) {
                bool startsWord = at == 0 || !isWordByte(static_cast<unsigned char>(text[at - 1]));
                size_t end = at + needle.size();
                bool endsWord = end >= text.size() || !isWordByte(static_cast<unsigned char>(text[end]));
                wholeWord = startsWord && endsWord;
            }
            if (wholeWord) {
                found.push_back({{line, at}, {line, at + needle.size()}});
            }
            at = text.find(needle, at + needle.size());
        }
    }
    if (found.empty()) {
        return;
    }
    document_->selection = found.front();
    document_->extraSelections.assign(found.begin() + 1, found.end());
    normalizeCursors();
    selectionDidChange();
}

void EditorView::selectNextOccurrence() {
    if (!document_) {
        return;
    }
    const auto& buffer = document_->buffer();
    Selection selection = document_->selection;
    if (selection.empty()) {
        std::string_view text = buffer.line(selection.caret.line);
        size_t start = selection.caret.column, end = selection.caret.column;
        while (start > 0 && isWordByte(static_cast<unsigned char>(text[start - 1]))) {
            --start;
        }
        while (end < text.size() && isWordByte(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        if (start == end) {
            beep();
            return;
        }
        selectRange({{selection.caret.line, start}, {selection.caret.line, end}});
        return;
    }
    Range range = selection.range();
    if (range.start.line != range.end.line) {
        beep();
        return;
    }
    std::string needle = buffer.text(range);
    Position searchFrom = range.end;
    for (const Selection& extra : document_->extraSelections) {
        if (searchFrom < extra.range().end) {
            searchFrom = extra.range().end;
        }
    }
    size_t lineCount = buffer.lineCount();
    for (size_t step = 0; step <= lineCount; ++step) {
        size_t line = (searchFrom.line + step) % lineCount;
        std::string_view text = buffer.line(line);
        size_t from = step == 0 ? searchFrom.column : 0;
        size_t found = text.find(needle, from);
        if (found == std::string::npos) {
            continue;
        }
        Selection match{{line, found}, {line, found + needle.size()}};
        bool alreadySelected = selectionsOverlap(match, selection);
        for (const Selection& extra : document_->extraSelections) {
            alreadySelected = alreadySelected || selectionsOverlap(match, extra);
        }
        if (alreadySelected) {
            beep();
            return;
        }
        document_->extraSelections.push_back(selection);
        document_->selection = match;
        document_->preferredX.reset();
        selectionDidChange();
        return;
    }
    beep();
}

// ---------------------------------------------------------------------------
// Clipboard and undo

std::string EditorView::copyTextOfSelection(const Selection& selection) const {
    if (selection.empty()) {
        return std::string(document_->buffer().line(selection.caret.line)) + "\n";
    }
    return document_->buffer().text(selection.range());
}

void EditorView::copy() {
    if (!document_) {
        return;
    }
    std::string text;
    if (document_->extraSelections.empty()) {
        text = copyTextOfSelection(document_->selection);
    } else {
        std::vector<Selection> all = selectionsInOrder();
        for (size_t index = 0; index < all.size(); ++index) {
            std::string piece = copyTextOfSelection(all[index]);
            if (!piece.empty() && piece.back() == '\n' && all[index].empty()) {
                piece.pop_back();
            }
            if (index > 0) {
                text += "\n";
            }
            text += piece;
        }
    }
    setClipboardText(text);
}

void EditorView::deleteSelectionOrLineForCut() {
    Selection selection = document_->selection;
    if (selection.empty()) {
        size_t line = selection.caret.line;
        const auto& buffer = document_->buffer();
        Range range = line + 1 < buffer.lineCount() ? Range{{line, 0}, {line + 1, 0}}
                      : line > 0 ? Range{{line - 1, buffer.line(line - 1).size()}, {line, buffer.line(line).size()}}
                                 : Range{{0, 0}, {0, buffer.line(0).size()}};
        document_->replace(range, "");
    } else {
        document_->replace(selection.range(), "");
    }
}

void EditorView::cut() {
    if (!document_ || document_->readOnly) {
        return;
    }
    copy();
    if (multiDepth_ == 0 && !document_->extraSelections.empty()) {
        forEachSelection([&] {
            deleteSelectionOrLineForCut();
            didEdit();
        }, false);
        return;
    }
    deleteSelectionOrLineForCut();
    didEdit();
}

void EditorView::paste() {
    if (!document_ || document_->readOnly) {
        return;
    }
    std::string text = clipboardText();
    if (text.empty()) {
        return;
    }
    if (multiDepth_ == 0 && !document_->extraSelections.empty()) {
        // As many lines as cursors: one line each, in order. Otherwise every cursor gets it all.
        std::vector<std::string> lines;
        size_t start = 0;
        while (true) {
            size_t end = text.find('\n', start);
            lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        if (lines.size() > 1 && lines.back().empty()) {
            lines.pop_back();
        }
        bool distribute = lines.size() == document_->extraSelections.size() + 1;
        forEachSelection([&] {
            const std::string& piece = distribute ? lines[cursorIndexInLoop_] : text;
            document_->replace(document_->selection.range(), piece);
            didEdit();
        }, false);
        return;
    }
    document_->replace(document_->selection.range(), text);
    didEdit();
}

void EditorView::undo() {
    if (document_ && document_->undo()) {
        didEdit();
    }
}

void EditorView::redo() {
    if (document_ && document_->redo()) {
        didEdit();
    }
}

bool EditorView::canUndo() const { return document_ && document_->canUndo(); }
bool EditorView::canRedo() const { return document_ && document_->canRedo(); }

}  // namespace gw::app
