#include "Popups.hpp"

#include "Font.hpp"
#include "PixelArt.hpp"
#include "Services.hpp"

#include <algorithm>
#include <cmath>

namespace gw::app {

namespace {

int Pixel() { return Services::shared().chromePixel(); }
int Cell() { return 6 * Pixel(); }
int Line() { return 12 * Pixel(); }
int Shadow() { return 4 * Pixel(); }
ui::Font& Chrome() { return Services::shared().chromeFont(); }
const bitmap::Scheme& Colors() { return Services::shared().scheme(); }

std::string trimmed(const std::string& text) {
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return std::string();
    }
    size_t end = text.find_last_not_of(" \t");
    return text.substr(start, end - start + 1);
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// Boxes of text

void RetroTextBox::clear() {
    lines_.clear();
    laidOut_.clear();
    bandLines_ = 0;
}

void RetroTextBox::addLine(const std::string& text, Color color, int indent) {
    lines_.push_back({text, color, indent});
}

bool RetroTextBox::addMarkdown(const std::string& markdown, Color color) {
    struct Plain {
        std::string text;
        bool code;
    };
    std::vector<Plain> plain;
    bool inCode = false;
    size_t start = 0;
    while (start <= markdown.size()) {
        size_t end = markdown.find('\n', start);
        if (end == std::string::npos) {
            end = markdown.size();
        }
        std::string line = replaceAll(markdown.substr(start, end - start), "\t", "    ");
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        start = end + 1;
        std::string clean = trimmed(line);
        if (clean.rfind("```", 0) == 0) {
            inCode = !inCode;
            if (end == markdown.size()) {
                break;
            }
            continue;
        }
        if (!inCode) {
            if (clean == "---" || clean == "***") {
                line.clear();
            }
            while (!line.empty() && line[0] == '#') {
                line.erase(line.begin());
            }
            for (const char* mark : {"**", "__", "`"}) {
                line = replaceAll(line, mark, "");
            }
            line = trimmed(line);
        }
        if (line.empty() && (plain.empty() || plain.back().text.empty())) {
            if (end == markdown.size()) {
                break;
            }
            continue;
        }
        plain.push_back({line, inCode});
        if (end == markdown.size()) {
            break;
        }
    }
    while (!plain.empty() && plain.back().text.empty()) {
        plain.pop_back();
    }
    for (const Plain& line : plain) {
        addLine(line.text, color, line.code ? 0 : 1);
    }
    return !plain.empty() && plain.front().code;
}

ui::Rect RetroTextBox::layout(int columns, int maxLines) {
    laidOut_.clear();
    for (const TextLine& line : lines_) {
        int room = std::max(1, columns - line.indent);
        for (const std::string& piece : ui::wrapText(line.text, static_cast<size_t>(room))) {
            if (static_cast<int>(laidOut_.size()) >= maxLines) {
                break;
            }
            laidOut_.push_back({piece, line.color, line.indent});
        }
    }
    int cell = Cell();
    int width = 0;
    for (const TextLine& line : laidOut_) {
        width = std::max(width, line.indent * cell + ui::textWidth(Chrome(), line.text));
    }
    int inset = 4 * Pixel();
    width_ = width + 2 * cell + 2 * inset;
    height_ = static_cast<int>(laidOut_.size()) * Line() + 2 * inset;
    return {0, 0, width_ + Shadow(), height_ + Shadow()};
}

void RetroTextBox::draw(ui::Canvas& canvas, int x, int y) {
    const bitmap::Scheme& colors = Colors();
    int pixel = Pixel(), cell = Cell(), line = Line();
    ui::Rect body{x, y, width_, height_};
    ui::drawRetroBox(canvas, body, colors.bar, colors.barText, pixel, Shadow());
    int inset = 4 * pixel;
    if (bandLines_ > 0) {
        int band = static_cast<int>(std::min(bandLines_, laidOut_.size())) * line;
        canvas.fill({x + inset, y + inset, width_ - 2 * inset, band}, colors.dialogTitle);
    }
    int top = y + inset;
    for (size_t index = 0; index < laidOut_.size(); ++index) {
        const TextLine& entry = laidOut_[index];
        Color color = index < bandLines_ ? colors.dialogTitleText : entry.color;
        ui::drawText(canvas, Chrome(), x + inset + cell * (1 + entry.indent), top, entry.text, color, false);
        top += line;
    }
}

// ---------------------------------------------------------------------------
// The completion list

void CompletionPopup::setItems(std::vector<std::shared_ptr<Suggestion>> items) {
    all_ = std::move(items);
    shown_ = all_;
    candidates_.clear();
    for (const auto& item : all_) {
        CompletionCandidate candidate;
        candidate.label = item->label;
        candidate.filterText = item->filterText;
        candidate.sortText = item->sortText;
        candidate.kind = item->kind;
        candidate.deprecated = item->deprecated;
        candidate.preselect = item->preselect;
        candidates_.push_back(std::move(candidate));
    }
    selected_ = 0;
    scroll_ = 0;
}

void CompletionPopup::show(const ui::Rect& caret, const ui::Rect& window) {
    caret_ = caret;
    window_ = window;
    visible_ = !shown_.empty();
    selectionChanged();
}

void CompletionPopup::filter(const std::string& prefix) {
    std::vector<size_t> order = rankCompletions(prefix, candidates_, history_, 200);
    shown_.clear();
    for (size_t index : order) {
        shown_.push_back(all_[index]);
    }
    selected_ = 0;
    scroll_ = 0;
    if (shown_.empty()) {
        visible_ = false;
        return;
    }
    selectionChanged();
}

void CompletionPopup::hide() {
    visible_ = false;
}

std::shared_ptr<Suggestion> CompletionPopup::selected() const {
    return visible_ && selected_ < shown_.size() ? shown_[selected_] : nullptr;
}

void CompletionPopup::recordAccepted(const Suggestion& item) {
    history_.accepted(item.label, item.kind);
}

void CompletionPopup::selectionChanged() {
    if (onSelectionChanged && selected_ < shown_.size()) {
        onSelectionChanged(shown_[selected_]);
    }
}

ui::Rect CompletionPopup::listRect() const {
    int line = Line(), pixel = Pixel();
    int rows = static_cast<int>(std::min<size_t>(shown_.size(), 10));
    int width = std::min(window_.w - 2 * Cell(), static_cast<int>(std::lround(400 * Services::shared().displayScale())));
    int height = rows * line + 8 * pixel;
    int x = std::min(caret_.x, window_.right() - width - Shadow());
    int y = caret_.bottom();
    if (y + height + Shadow() > window_.bottom()) {
        y = caret_.y - height - Shadow();  // no room below: above the caret
    }
    return {std::max(window_.x, x), std::max(window_.y, y), width, height};
}

bool CompletionPopup::contains(int x, int y) const {
    return visible_ && listRect().contains(x, y);
}

bool CompletionPopup::handleKey(const std::string& action) {
    if (!visible_ || shown_.empty()) {
        return false;
    }
    auto select = [&](long index) {
        selected_ = static_cast<size_t>(std::clamp(index, 0L, static_cast<long>(shown_.size()) - 1));
        if (selected_ < scroll_) {
            scroll_ = selected_;
        } else if (selected_ >= scroll_ + 10) {
            scroll_ = selected_ - 9;
        }
        selectionChanged();
    };
    if (action == "up") {
        select(selected_ == 0 ? static_cast<long>(shown_.size()) - 1 : static_cast<long>(selected_) - 1);
    } else if (action == "down") {
        select(selected_ + 1 >= shown_.size() ? 0 : static_cast<long>(selected_) + 1);
    } else if (action == "pageup") {
        select(static_cast<long>(selected_) - 10);
    } else if (action == "pagedown") {
        select(static_cast<long>(selected_) + 10);
    } else if (action == "enter" || action == "tab") {
        std::shared_ptr<Suggestion> item = shown_[selected_];
        hide();
        if (onAccept) {
            onAccept(item);
        }
    } else if (action == "escape") {
        hide();
    } else {
        return false;
    }
    return true;
}

void CompletionPopup::draw(ui::Canvas& canvas) {
    if (!visible_ || shown_.empty()) {
        return;
    }
    const bitmap::Scheme& colors = Colors();
    int pixel = Pixel(), cell = Cell(), line = Line();
    ui::Rect box = listRect();
    ui::drawRetroBox(canvas, box, colors.bar, colors.barText, pixel, Shadow());
    int top = box.y + 4 * pixel;
    int cells = (box.w - 8 * pixel) / cell;
    for (size_t index = scroll_; index < shown_.size() && index < scroll_ + 10; ++index) {
        const Suggestion& item = *shown_[index];
        bool selected = index == selected_;
        ui::Rect row{box.x + 4 * pixel, top, box.w - 8 * pixel, line};
        if (selected) {
            canvas.fill(row, colors.barHighlight);
        }
        Color color = item.deprecated ? colors.barDim : selected ? colors.barHighlightText : colors.barText;
        std::string label = item.label;
        int labelCells = ui::textCells(label);
        int detailRoom = cells - labelCells - 3;
        canvas.pushClip(row);
        ui::drawText(canvas, Chrome(), row.x + cell, top, label, color, false);
        if (item.deprecated) {
            canvas.fill({row.x + cell, top + line / 2, labelCells * cell, pixel}, color);
        }
        if (detailRoom > 3 && !item.detail.empty()) {
            std::string detail = item.detail.substr(0, item.detail.find('\n'));
            if (ui::textCells(detail) > detailRoom) {
                detail = detail.substr(0, static_cast<size_t>(detailRoom - 1)) + "…";
            }
            int width = ui::textWidth(Chrome(), detail);
            ui::drawText(canvas, Chrome(), row.right() - cell - width, top, detail, selected ? colors.barHighlightText : colors.barDim, false);
        }
        canvas.popClip();
        top += line;
    }
    // The chosen item's documentation beside the list, its signature on the title strip.
    const Suggestion& chosen = *shown_[selected_];
    if (!chosen.documentation.empty() || !chosen.detail.empty()) {
        RetroTextBox documentation;
        if (!chosen.detail.empty()) {
            documentation.addLine(chosen.detail, colors.barText, 0);
            documentation.setBandLines(1);
        }
        documentation.addMarkdown(chosen.documentation, colors.barText);
        ui::Rect size = documentation.layout(44, 24);
        int x = box.right() + Shadow() + cell / 2;
        if (x + size.w > window_.right()) {
            x = box.x - size.w - cell / 2;
        }
        if (x >= window_.x) {
            documentation.draw(canvas, x, box.y);
        }
    }
}

bool CompletionPopup::mouseDown(const MouseEvent& event) {
    if (!contains(event.x, event.y)) {
        return false;
    }
    ui::Rect box = listRect();
    int row = (event.y - box.y - 4 * Pixel()) / Line();
    size_t index = scroll_ + static_cast<size_t>(std::max(0, row));
    if (index < shown_.size()) {
        if (index == selected_ || event.clicks >= 2) {
            selected_ = index;
            handleKey("enter");
        } else {
            selected_ = index;
            selectionChanged();
        }
    }
    return true;
}

bool CompletionPopup::wheel(const WheelEvent& event) {
    if (!contains(event.x, event.y) || shown_.size() <= 10) {
        return false;
    }
    long next = static_cast<long>(scroll_) - static_cast<long>(std::lround(event.dy * 3));
    scroll_ = static_cast<size_t>(std::clamp(next, 0L, static_cast<long>(shown_.size()) - 10));
    return true;
}

// ---------------------------------------------------------------------------
// Hover and signatures

void HoverBox::show(const std::string& markdown, const std::vector<std::string>& problems, const ui::Rect& word, const ui::Rect& window) {
    const bitmap::Scheme& colors = Colors();
    box_.clear();
    signature_ = false;
    for (const std::string& problem : problems) {
        box_.addLine(problem, colors.barHotkey, 0);
    }
    std::string text = markdown.size() > 2400 ? markdown.substr(0, 2400) + "\n\n…" : markdown;
    if (!text.empty() && box_.addMarkdown(text, colors.barText) && problems.empty()) {
        box_.setBandLines(1);  // the declaration on the title strip
    }
    if (box_.empty()) {
        visible_ = false;
        return;
    }
    ui::Rect size = box_.layout(56, 20);
    int x = std::max(window.x + 4, std::min(word.x, window.right() - size.w - 4));
    int y = word.bottom() + 2 * Pixel();
    if (y + size.h > window.bottom()) {
        y = word.y - size.h - 2 * Pixel();
    }
    rect_ = {x, std::max(window.y, y), size.w, size.h};
    visible_ = true;
}

void HoverBox::showSignature(const std::string& signature, int activeStart, int activeEnd, const std::string& documentation,
                             const ui::Rect& caret, const ui::Rect& window) {
    const bitmap::Scheme& colors = Colors();
    box_.clear();
    signature_ = true;
    signatureText_ = signature;
    activeStart_ = activeStart;
    activeEnd_ = activeEnd;
    box_.addLine(signature, colors.barText, 0);
    if (!documentation.empty()) {
        box_.addMarkdown(documentation, colors.barDim);
    }
    ui::Rect size = box_.layout(std::min(90, std::max(20, ui::textCells(signature) + 2)), 8);
    int x = std::max(window.x + 4, std::min(caret.x, window.right() - size.w - 4));
    int y = caret.y - size.h - Pixel();
    if (y < window.y) {
        y = caret.bottom() + Pixel();
    }
    rect_ = {x, y, size.w, size.h};
    visible_ = true;
}

void HoverBox::draw(ui::Canvas& canvas) {
    if (!visible_) {
        return;
    }
    box_.draw(canvas, rect_.x, rect_.y);
    if (signature_ && activeStart_ >= 0 && activeEnd_ > activeStart_ && ui::textCells(signatureText_) <= 88) {
        // The parameter being typed, in the hotkey color over the plain first line.
        int inset = 4 * Pixel();
        int x = rect_.x + inset + Cell();
        int y = rect_.y + inset;
        std::string before = signatureText_.substr(0, static_cast<size_t>(activeStart_));
        std::string active = signatureText_.substr(static_cast<size_t>(activeStart_), static_cast<size_t>(activeEnd_ - activeStart_));
        ui::drawText(canvas, Chrome(), x + ui::textWidth(Chrome(), before), y, active, Colors().barHotkey, false);
    }
}

// ---------------------------------------------------------------------------
// Change bars

void ChangeBox::show(const std::string& title, const std::vector<std::string>& oldLines, const std::vector<std::string>& newLines,
                     const ui::Rect& bar, const ui::Rect& window) {
    const bitmap::Scheme& colors = Colors();
    title_ = title;
    box_.clear();
    box_.addLine(title, colors.barText, 0);
    box_.setBandLines(1);
    size_t shown = 0;
    for (const std::string& line : oldLines) {
        if (++shown > 14) {
            box_.addLine("…", colors.barDim, 0);
            break;
        }
        box_.addLine("- " + line, bitmap::kRed, 0);
    }
    shown = 0;
    for (const std::string& line : newLines) {
        if (++shown > 14) {
            box_.addLine("…", colors.barDim, 0);
            break;
        }
        box_.addLine("+ " + line, bitmap::kGreen, 0);
    }
    box_.addLine("", colors.barText, 0);
    box_.addLine("[Revert]  [Stage]  [<] [>]", colors.barText, 0);
    ui::Rect size = box_.layout(70, 34);
    int x = std::min(bar.right() + Cell(), window.right() - size.w);
    int y = std::min(bar.y, window.bottom() - size.h);
    rect_ = {std::max(window.x, x), std::max(window.y, y), size.w, size.h};
    visible_ = true;
}

void ChangeBox::draw(ui::Canvas& canvas) {
    if (!visible_) {
        return;
    }
    box_.draw(canvas, rect_.x, rect_.y);
    // The buttons are on the last line: find each label's place in it.
    int inset = 4 * Pixel(), cell = Cell(), line = Line();
    int y = rect_.y + rect_.h - Shadow() - inset - line;
    int x = rect_.x + inset + cell;
    buttons_.clear();
    const char* labels[] = {"[Revert]", "[Stage]", "[<]", "[>]"};
    int offsets[] = {0, 10, 19, 23};
    for (int index = 0; index < 4; ++index) {
        int width = ui::textWidth(Chrome(), labels[index]);
        buttons_.push_back({{x + offsets[index] * cell, y, width, line}, index});
    }
}

bool ChangeBox::mouseDown(const MouseEvent& event) {
    if (!visible_) {
        return false;
    }
    if (!rect_.contains(event.x, event.y)) {
        visible_ = false;
        return false;
    }
    for (const Button& button : buttons_) {
        if (!button.rect.contains(event.x, event.y)) {
            continue;
        }
        std::function<void()> action;
        switch (button.action) {
            case 0: action = onRevert; break;
            case 1: action = onStage; break;
            case 2: action = onPrevious; break;
            case 3: action = onNext; break;
            default: break;
        }
        visible_ = false;
        if (action) {
            action();
        }
        return true;
    }
    return true;
}

}  // namespace gw::app
