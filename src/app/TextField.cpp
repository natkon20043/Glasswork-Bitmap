#include "TextField.hpp"

#include "Platform.hpp"
#include "TextBuffer.hpp"

#include <algorithm>

namespace gw::app {

void TextField::setText(const std::string& text, bool selectEverything) {
    text_ = text;
    caret_ = text_.size();
    anchor_ = selectEverything ? 0 : caret_;
    scrollCells_ = 0;
}

void TextField::selectAll() {
    anchor_ = 0;
    caret_ = text_.size();
}

size_t TextField::previousBoundary(size_t index) const {
    if (index == 0) {
        return 0;
    }
    --index;
    while (index > 0 && (static_cast<unsigned char>(text_[index]) & 0xC0) == 0x80) {
        --index;
    }
    return index;
}

size_t TextField::nextBoundary(size_t index) const {
    if (index >= text_.size()) {
        return text_.size();
    }
    ++index;
    while (index < text_.size() && (static_cast<unsigned char>(text_[index]) & 0xC0) == 0x80) {
        ++index;
    }
    return index;
}

size_t TextField::previousWord(size_t index) const {
    while (index > 0 && !isWordByte(static_cast<unsigned char>(text_[index - 1]))) {
        --index;
    }
    while (index > 0 && isWordByte(static_cast<unsigned char>(text_[index - 1]))) {
        --index;
    }
    return index;
}

size_t TextField::nextWord(size_t index) const {
    while (index < text_.size() && !isWordByte(static_cast<unsigned char>(text_[index]))) {
        ++index;
    }
    while (index < text_.size() && isWordByte(static_cast<unsigned char>(text_[index]))) {
        ++index;
    }
    return index;
}

void TextField::changed() {
    if (onChange) {
        onChange();
    }
}

void TextField::replaceSelection(const std::string& text) {
    undoText_ = text_;
    undoCaret_ = caret_;
    size_t start = std::min(caret_, anchor_), end = std::max(caret_, anchor_);
    std::string cleaned;
    for (char character : text) {
        cleaned += (character == '\n' || character == '\r') ? ' ' : character;
    }
    text_.replace(start, end - start, cleaned);
    caret_ = anchor_ = start + cleaned.size();
    changed();
}

void TextField::insert(const std::string& text) {
    replaceSelection(text);
}

bool TextField::keyDown(const KeyPress& key) {
    bool extend = key.shift;
    auto move = [&](size_t to) {
        caret_ = to;
        if (!extend) {
            anchor_ = caret_;
        }
    };
    bool word = key.control;
    if (key.key == "left" && !key.alt) {
        if (!extend && caret_ != anchor_ && !word) {
            move(std::min(caret_, anchor_));
        } else {
            move(word ? previousWord(caret_) : previousBoundary(caret_));
        }
        return true;
    }
    if (key.key == "right" && !key.alt) {
        if (!extend && caret_ != anchor_ && !word) {
            move(std::max(caret_, anchor_));
        } else {
            move(word ? nextWord(caret_) : nextBoundary(caret_));
        }
        return true;
    }
    if (key.key == "home") {
        move(0);
        return true;
    }
    if (key.key == "end") {
        move(text_.size());
        return true;
    }
    if (key.key == "backspace") {
        if (caret_ == anchor_) {
            anchor_ = word ? previousWord(caret_) : previousBoundary(caret_);
        }
        replaceSelection("");
        return true;
    }
    if (key.key == "delete" && !key.shift) {
        if (caret_ == anchor_) {
            anchor_ = word ? nextWord(caret_) : nextBoundary(caret_);
        }
        replaceSelection("");
        return true;
    }
    if (key.control && !key.alt) {
        size_t start = std::min(caret_, anchor_), end = std::max(caret_, anchor_);
        if (key.key == "a") {
            selectAll();
            return true;
        }
        if (key.key == "c" || key.key == "insert") {
            if (end > start) {
                setClipboardText(text_.substr(start, end - start));
            }
            return true;
        }
        if (key.key == "x") {
            if (end > start) {
                setClipboardText(text_.substr(start, end - start));
                replaceSelection("");
            }
            return true;
        }
        if (key.key == "v") {
            replaceSelection(clipboardText());
            return true;
        }
        if (key.key == "z") {
            std::swap(text_, undoText_);
            caret_ = anchor_ = std::min(undoCaret_, text_.size());
            changed();
            return true;
        }
    }
    if (key.shift && key.key == "insert") {
        replaceSelection(clipboardText());
        return true;
    }
    return false;
}

void TextField::clickAt(int x, int cellWidth, bool extend) {
    int cell = std::max(0, (x + cellWidth / 2) / std::max(1, cellWidth)) + scrollCells_;
    size_t index = 0;
    int cells = 0;
    while (index < text_.size()) {
        size_t next = index;
        char32_t character = ui::decodeUtf8(text_, next);
        int width = ui::Font::cells(character);
        if (cells + width > cell) {
            break;
        }
        cells += width;
        index = next;
    }
    caret_ = index;
    if (!extend) {
        anchor_ = caret_;
    }
}

void TextField::draw(ui::Canvas& canvas, ui::Font& font, int x, int y, int width, ui::Color text, ui::Color selection,
                     ui::Color selectionText, ui::Color caret, bool focused, bool shadow, const std::string& placeholder,
                     ui::Color placeholderColor) {
    int cellWidth = font.cellWidth();
    int visibleCells = std::max(1, width / cellWidth);
    int caretCells = ui::textCells(std::string_view(text_).substr(0, caret_));
    // Keep the caret in view, scrolling by whole characters.
    if (caretCells - scrollCells_ >= visibleCells) {
        scrollCells_ = caretCells - visibleCells + 1;
    }
    if (caretCells < scrollCells_) {
        scrollCells_ = caretCells;
    }
    canvas.pushClip({x, y, width, font.lineHeight()});
    int left = x - scrollCells_ * cellWidth;
    if (text_.empty() && !placeholder.empty()) {
        ui::drawText(canvas, font, x, y, placeholder, placeholderColor, false);
    }
    size_t start = std::min(caret_, anchor_), end = std::max(caret_, anchor_);
    if (end > start && focused) {
        int from = ui::textCells(std::string_view(text_).substr(0, start));
        int to = ui::textCells(std::string_view(text_).substr(0, end));
        canvas.fill({left + from * cellWidth, y, (to - from) * cellWidth, font.lineHeight()}, selection);
    }
    int cursor = left;
    for (size_t index = 0; index < text_.size();) {
        size_t at = index;
        char32_t character = ui::decodeUtf8(text_, index);
        bool selected = focused && at >= start && at < end;
        ui::drawGlyph(canvas, font, character, cursor, y, selected ? selectionText : text, shadow && !selected);
        cursor += ui::Font::cells(character) * cellWidth;
    }
    if (focused) {
        int caretX = left + caretCells * cellWidth;
        canvas.fill({caretX, y + font.pixel(), std::max(1, font.pixel()), font.lineHeight() - 2 * font.pixel()}, caret);
    }
    canvas.popClip();
}

}  // namespace gw::app
