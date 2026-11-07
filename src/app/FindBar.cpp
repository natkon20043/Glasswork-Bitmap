#include "FindBar.hpp"

#include "EditorView.hpp"
#include "Font.hpp"
#include "PixelArt.hpp"
#include "Platform.hpp"
#include "Services.hpp"

#include <algorithm>

namespace gw::app {

namespace {

int Pixel() { return Services::shared().chromePixel(); }
int Cell() { return 6 * Pixel(); }
int Line() { return 12 * Pixel(); }
int Shadow() { return 4 * Pixel(); }
ui::Font& Chrome() { return Services::shared().chromeFont(); }

enum Action { kCase = 1, kWord, kRegex, kPrevious, kNext, kClose, kToggleReplace, kReplace, kReplaceAll };

}  // namespace

void FindBar::show(const std::string& text, bool replace) {
    visible_ = true;
    if (!text.empty() && text.find('\n') == std::string::npos) {
        find_.setText(text, true);
    } else {
        find_.selectAll();
    }
    replaceVisible_ = replace || replaceVisible_;
    if (!replace) {
        replaceVisible_ = false;
    }
    replaceFocused_ = false;
    Document* document = editor_.document();
    anchor_ = document ? document->selection.range().start : Position{};
    recompute(true);
    changed();
}

void FindBar::close() {
    if (!visible_) {
        return;
    }
    visible_ = false;
    editor_.setFindMatches({}, -1);
    if (onClose) {
        onClose();
    }
    changed();
}

void FindBar::setSearchText(const std::string& text) {
    find_.setText(text);
    if (visible_) {
        recompute(true);
    }
}

void FindBar::changed() {
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
}

search::Query FindBar::query() const {
    search::Query query;
    query.pattern = find_.text();
    query.caseSensitive = caseSensitive_;
    query.wholeWord = wholeWord_;
    query.useRegex = regex_;
    return query;
}

void FindBar::documentChanged() {
    if (!visible_) {
        return;
    }
    if (Document* document = editor_.document()) {
        anchor_ = document->buffer().clamp(anchor_);
    }
    recompute(false);
}

void FindBar::recompute(bool select) {
    matches_.clear();
    current_ = -1;
    countIsProblem_ = false;
    Document* document = editor_.document();
    matcher_ = std::make_unique<search::Matcher>(query());
    if (document == nullptr || find_.text().empty()) {
        count_.clear();
        editor_.setFindMatches({}, -1);
        changed();
        return;
    }
    if (!matcher_->valid()) {
        count_ = "Invalid";
        countIsProblem_ = true;
        editor_.setFindMatches({}, -1);
        changed();
        return;
    }
    const TextBuffer& buffer = document->buffer();
    const size_t limit = 50000;
    for (size_t line = 0; line < buffer.lineCount() && matches_.size() < limit; ++line) {
        matcher_->forEachInLine(buffer.line(line), [&](size_t start, size_t length) {
            if (matches_.size() < limit) {
                matches_.push_back({{line, start}, {line, start + length}});
            }
        });
    }
    if (!matches_.empty()) {
        current_ = 0;
        for (size_t index = 0; index < matches_.size(); ++index) {
            if (!(matches_[index].start < anchor_)) {
                current_ = static_cast<long>(index);
                break;
            }
        }
    }
    if (matches_.empty()) {
        count_ = "No results";
        countIsProblem_ = true;
    } else {
        count_ = std::to_string(current_ + 1) + " of " + (matches_.size() >= limit ? std::string("50000+") : std::to_string(matches_.size()));
    }
    editor_.setFindMatches(matches_, current_);
    if (select && current_ >= 0) {
        editor_.selectRange(matches_[static_cast<size_t>(current_)]);
    }
    changed();
}

void FindBar::step(int direction) {
    if (!visible_) {
        // F3 with the bar closed: the last search, from the caret, without showing anything.
        Document* document = editor_.document();
        anchor_ = Position{0, 0};
        if (document != nullptr) {
            Range selected = document->selection.range();
            anchor_ = direction > 0 ? selected.end : selected.start;
        }
        recompute(false);
        if (matches_.empty()) {
            beep();
            return;
        }
        long count = static_cast<long>(matches_.size());
        if (direction > 0) {
            current_ = -1;
            for (long index = 0; index < count; ++index) {
                if (!(matches_[static_cast<size_t>(index)].start < anchor_)) {
                    current_ = index;
                    break;
                }
            }
            if (current_ < 0) {
                current_ = 0;
            }
        } else {
            current_ = count - 1;
            for (long index = count - 1; index >= 0; --index) {
                if (matches_[static_cast<size_t>(index)].start < anchor_) {
                    current_ = index;
                    break;
                }
            }
        }
    } else {
        if (matches_.empty()) {
            beep();
            return;
        }
        long count = static_cast<long>(matches_.size());
        current_ = (current_ + direction + count) % count;
    }
    count_ = std::to_string(current_ + 1) + " of " + std::to_string(matches_.size());
    editor_.setFindMatches(matches_, current_);
    editor_.selectRange(matches_[static_cast<size_t>(current_)]);
    anchor_ = matches_[static_cast<size_t>(current_)].start;
    changed();
}

std::string FindBar::replacementFor(const Range& match, Document& document) {
    std::string_view line = document.buffer().line(match.start.line);
    return matcher_->expand(line, match.start.column, match.end.column - match.start.column, replace_.text());
}

void FindBar::replaceOne() {
    Document* document = editor_.document();
    if (document == nullptr || matches_.empty() || current_ < 0 || document->readOnly) {
        beep();
        return;
    }
    Range match = matches_[static_cast<size_t>(current_)];
    std::string replacement = replacementFor(match, *document);
    editor_.editDocument([&](Document& doc) { doc.replace(match, replacement); });
    anchor_ = endOfInsertion(match.start, replacement);
    recompute(true);
}

void FindBar::replaceAll() {
    Document* document = editor_.document();
    if (document == nullptr || matches_.empty() || document->readOnly) {
        beep();
        return;
    }
    std::vector<Range> matches = matches_;
    std::vector<std::string> replacements;
    for (const Range& match : matches) {
        replacements.push_back(replacementFor(match, *document));
    }
    // From the end backwards, so earlier positions stay valid. One undo step.
    editor_.editDocument([&](Document& doc) {
        for (size_t index = matches.size(); index-- > 0;) {
            doc.replace(matches[index], replacements[index]);
        }
    });
    recompute(false);
}

// ---------------------------------------------------------------------------
// Layout and drawing

void FindBar::layout(const ui::Rect& editorArea) {
    int cell = Cell(), line = Line(), pixel = Pixel();
    int width = std::min(editorArea.w - 2 * cell, 64 * cell);
    width = std::max(width, 30 * cell);
    int rows = replaceVisible_ ? 2 : 1;
    int height = 8 * pixel + rows * line + (rows - 1) * 2 * pixel;
    int x = editorArea.right() - width - 2 * cell - Shadow();
    box_ = {x / pixel * pixel, (editorArea.y + pixel * 2) / pixel * pixel, width, height};
}

void FindBar::draw(ui::Canvas& canvas, bool focused) {
    if (!visible_) {
        return;
    }
    const bitmap::Scheme& colors = Services::shared().scheme();
    int pixel = Pixel(), cell = Cell(), line = Line();
    ui::drawRetroBox(canvas, box_, colors.bar, colors.barText, pixel, Shadow());
    buttons_.clear();
    int top = box_.y + 4 * pixel;
    int right = box_.right() - 4 * pixel - cell / 2;
    auto button = [&](const std::string& label, int action, bool on, int y) {
        int width = ui::textWidth(Chrome(), label);
        right -= width;
        ui::Rect rect{right, y, width, line};
        if (on) {
            canvas.fill(rect, colors.barHighlight);
        }
        ui::drawTextPlain(canvas, Chrome(), right, y, label, on ? colors.barHighlightText : colors.barText);
        buttons_.push_back({rect, action});
        right -= cell / 2;
    };
    // First row: [>] toggle, the field, the count, Aa W .* < > x
    button("x", kClose, false, top);
    button(">", kNext, false, top);
    button("<", kPrevious, false, top);
    right -= cell / 2;
    button(".*", kRegex, regex_, top);
    button("W", kWord, wholeWord_, top);
    button("Aa", kCase, caseSensitive_, top);
    int countWidth = ui::textWidth(Chrome(), count_);
    right -= cell / 2;
    if (!count_.empty()) {
        ui::drawTextPlain(canvas, Chrome(), right - countWidth, top, count_, countIsProblem_ ? colors.barHotkey : colors.barText);
    }
    int fieldRight = right - countWidth - cell;
    int left = box_.x + 4 * pixel + cell / 2;
    std::string toggle = replaceVisible_ ? "v" : ">";
    ui::drawTextPlain(canvas, Chrome(), left, top, toggle, colors.barText);
    buttons_.push_back({{left, top, cell, line}, kToggleReplace});
    int fieldLeft = left + 2 * cell;
    findField_ = {fieldLeft, top, std::max(cell * 4, fieldRight - fieldLeft), line};
    canvas.fill(findField_, colors.input);
    bool findFocused = focused && !replaceFocused_;
    find_.draw(canvas, Chrome(), findField_.x + cell / 2, findField_.y, findField_.w - cell, colors.inputText, colors.barHighlight,
               colors.barHighlightText, colors.inputText, findFocused, false, "Find", colors.barDim);
    if (replaceVisible_) {
        int second = top + line + 2 * pixel;
        right = box_.right() - 4 * pixel - cell / 2;
        button("[All]", kReplaceAll, false, second);
        button("[Replace]", kReplace, false, second);
        replaceField_ = {fieldLeft, second, std::max(cell * 4, right - cell / 2 - fieldLeft), line};
        canvas.fill(replaceField_, colors.input);
        replace_.draw(canvas, Chrome(), replaceField_.x + cell / 2, replaceField_.y, replaceField_.w - cell, colors.inputText,
                      colors.barHighlight, colors.barHighlightText, colors.inputText, focused && replaceFocused_, false, "Replace",
                      colors.barDim);
    } else {
        replaceField_ = {};
    }
}

bool FindBar::mouseDown(const MouseEvent& event) {
    if (!visible_ || !box_.inset(0, 0).contains(event.x, event.y)) {
        return false;
    }
    if (onFocus) {
        onFocus();
    }
    for (const Button& button : buttons_) {
        if (!button.rect.contains(event.x, event.y)) {
            continue;
        }
        switch (button.action) {
            case kCase: caseSensitive_ = !caseSensitive_; recompute(true); break;
            case kWord: wholeWord_ = !wholeWord_; recompute(true); break;
            case kRegex: regex_ = !regex_; recompute(true); break;
            case kPrevious: step(-1); break;
            case kNext: step(1); break;
            case kClose: close(); break;
            case kToggleReplace: replaceVisible_ = !replaceVisible_; replaceFocused_ = replaceVisible_ && replaceFocused_; changed(); break;
            case kReplace: replaceOne(); break;
            case kReplaceAll: replaceAll(); break;
            default: break;
        }
        return true;
    }
    if (findField_.contains(event.x, event.y)) {
        replaceFocused_ = false;
        find_.clickAt(event.x - findField_.x - Cell() / 2, Chrome().cellWidth(), event.shift);
    } else if (replaceVisible_ && replaceField_.contains(event.x, event.y)) {
        replaceFocused_ = true;
        replace_.clickAt(event.x - replaceField_.x - Cell() / 2, Chrome().cellWidth(), event.shift);
    }
    changed();
    return true;
}

bool FindBar::keyDown(const KeyPress& key) {
    if (!visible_) {
        return false;
    }
    if (key.key == "escape") {
        close();
        return true;
    }
    if (key.key == "enter") {
        if (replaceFocused_) {
            if (key.control && key.alt) {
                replaceAll();
            } else {
                replaceOne();
            }
        } else {
            step(key.shift ? -1 : 1);
        }
        return true;
    }
    if (key.key == "tab" && replaceVisible_) {
        replaceFocused_ = !replaceFocused_;
        changed();
        return true;
    }
    // Alt+C, Alt+W, Alt+R toggle the options, as in VS Code.
    if (key.alt && !key.control) {
        if (key.key == "c") {
            caseSensitive_ = !caseSensitive_;
        } else if (key.key == "w") {
            wholeWord_ = !wholeWord_;
        } else if (key.key == "r") {
            regex_ = !regex_;
        } else {
            return false;
        }
        recompute(true);
        return true;
    }
    TextField& field = replaceFocused_ ? replace_ : find_;
    std::string before = find_.text();
    if (field.keyDown(key)) {
        if (!replaceFocused_ && find_.text() != before) {
            recompute(true);
        }
        changed();
        return true;
    }
    return false;
}

void FindBar::textInput(const std::string& text) {
    if (replaceFocused_) {
        replace_.insert(text);
        changed();
        return;
    }
    find_.insert(text);
    recompute(true);
}

}  // namespace gw::app
