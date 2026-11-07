#include "Overlays.hpp"

#include "Font.hpp"
#include "Fuzzy.hpp"
#include "PixelArt.hpp"
#include "Services.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace gw::app {

namespace {

int Pixel() { return Services::shared().chromePixel(); }
int Cell() { return 6 * Pixel(); }
int Line() { return 12 * Pixel(); }
int Shadow() { return 4 * Pixel(); }
ui::Font& Chrome() { return Services::shared().chromeFont(); }
const bitmap::Scheme& Colors() { return Services::shared().scheme(); }

/// Lower-case letter of a hotkey, from the title's `hotkey`-th character.
std::string hotkeyLetter(const std::string& title, int hotkey) {
    int index = 0;
    for (size_t offset = 0; offset < title.size(); ++index) {
        size_t start = offset;
        ui::decodeUtf8(title, offset);
        if (index == hotkey) {
            std::string letter = title.substr(start, offset - start);
            for (char& c : letter) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return letter;
        }
    }
    return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Menus

MenuOverlay::MenuOverlay(std::vector<Menu> menus, int index, Host host) : menus_(std::move(menus)), host_(std::move(host)) {
    show(index);
    releaseChooses_ = false;
}

MenuOverlay::MenuOverlay(Menu menu, int x, int y, Host host) : host_(std::move(host)), atPoint_(true), pointX_(x), pointY_(y) {
    menus_.push_back(std::move(menu));
    show(0);
}

void MenuOverlay::layout(const ui::Rect& window) {
    window_ = window;
}

int MenuOverlay::itemHeight(const MenuItem& item) const {
    return item.isSeparator ? Line() / 2 : Line();
}

void MenuOverlay::show(int index) {
    if (menus_.empty()) {
        close();
        return;
    }
    index_ = (index % static_cast<int>(menus_.size()) + static_cast<int>(menus_.size())) % static_cast<int>(menus_.size());
    enabled_.clear();
    shortcuts_.clear();
    highlighted_ = -1;
    for (const MenuItem& item : menus_[index_].items) {
        bool enabled = !item.isSeparator && (host_.enabled ? host_.enabled(item) : true);
        enabled_.push_back(enabled);
        shortcuts_.push_back(item.isSeparator ? std::string() : (host_.shortcut ? host_.shortcut(item) : item.shortcut));
        if (enabled && highlighted_ < 0) {
            highlighted_ = static_cast<int>(enabled_.size()) - 1;
        }
    }
    if (host_.showing && !atPoint_) {
        host_.showing(index_);
    }
}

ui::Rect MenuOverlay::dropdownRect() const {
    const Menu& menu = menus_[index_];
    int titles = 0, shortcuts = 0, height = 0;
    for (size_t index = 0; index < menu.items.size(); ++index) {
        int hotkey = -1;
        std::string title = ui::withoutHotkeyMark(menu.items[index].title, &hotkey);
        titles = std::max(titles, ui::textWidth(Chrome(), title));
        shortcuts = std::max(shortcuts, ui::textWidth(Chrome(), shortcuts_[index]));
        height += itemHeight(menu.items[index]);
    }
    int cell = Cell();
    int width = 2 * cell + titles + (shortcuts > 0 ? 3 * cell + shortcuts : 0) + 2 * cell;
    int x = pointX_, y = pointY_;
    if (!atPoint_ && host_.titleRects) {
        std::vector<ui::Rect> titleRects = host_.titleRects();
        if (static_cast<size_t>(index_) < titleRects.size()) {
            x = titleRects[static_cast<size_t>(index_)].x;
            y = titleRects[static_cast<size_t>(index_)].bottom();
        }
    }
    int total = height + 8 * Pixel();
    x = std::max(0, std::min(x, window_.right() - width - Shadow()));
    if (atPoint_ && y + total + Shadow() > window_.bottom()) {
        y = std::max(0, window_.bottom() - total - Shadow());
    }
    return {x, y, width, total};
}

int MenuOverlay::rowAt(int x, int y) const {
    ui::Rect box = dropdownRect();
    if (!box.contains(x, y)) {
        return -1;
    }
    int top = box.y + 4 * Pixel();
    const Menu& menu = menus_[index_];
    for (size_t index = 0; index < menu.items.size(); ++index) {
        int height = itemHeight(menu.items[index]);
        if (y >= top && y < top + height) {
            return static_cast<int>(index);
        }
        top += height;
    }
    return -1;
}

int MenuOverlay::titleAt(int x, int y) const {
    if (atPoint_ || !host_.titleRects) {
        return -1;
    }
    std::vector<ui::Rect> titles = host_.titleRects();
    for (size_t index = 0; index < titles.size(); ++index) {
        if (titles[index].contains(x, y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void MenuOverlay::draw(ui::Canvas& canvas) {
    const bitmap::Scheme& colors = Colors();
    ui::Rect box = dropdownRect();
    int pixel = Pixel(), cell = Cell();
    ui::drawRetroBox(canvas, box, colors.bar, colors.barText, pixel, Shadow());
    int y = box.y + 4 * pixel;
    const Menu& menu = menus_[index_];
    for (size_t index = 0; index < menu.items.size(); ++index) {
        const MenuItem& item = menu.items[index];
        int height = itemHeight(item);
        if (item.isSeparator) {
            canvas.fill({box.x + 2 * pixel, y + height / 2 - pixel, box.w - 4 * pixel, pixel}, colors.barText);
            y += height;
            continue;
        }
        bool enabled = enabled_[index];
        bool highlighted = static_cast<int>(index) == highlighted_;
        if (highlighted) {
            canvas.fill({box.x + 4 * pixel, y, box.w - 8 * pixel, height}, colors.barHighlight);
        }
        Color color = !enabled ? colors.barDim : highlighted ? colors.barHighlightText : colors.barText;
        Color hotkeyColor = enabled ? colors.barHotkey : color;
        if (item.checked && item.checked()) {
            ui::drawGlyphPlain(canvas, Chrome(), U'•', box.x + cell, y, color);
        }
        int hotkey = -1;
        std::string title = ui::withoutHotkeyMark(item.title, &hotkey);
        ui::drawWithHotkey(canvas, Chrome(), box.x + 2 * cell, y, title, hotkey, color, hotkeyColor, false);
        if (!shortcuts_[index].empty()) {
            int width = ui::textWidth(Chrome(), shortcuts_[index]);
            ui::drawTextPlain(canvas, Chrome(), box.right() - 2 * cell - width, y, shortcuts_[index], color);
        }
        y += height;
    }
}

void MenuOverlay::moveHighlight(int step) {
    int count = static_cast<int>(enabled_.size());
    for (int tries = 0, row = highlighted_; tries < count; ++tries) {
        row = (row + step + count) % count;
        if (enabled_[static_cast<size_t>(row)]) {
            highlighted_ = row;
            return;
        }
    }
}

void MenuOverlay::choose(int row) {
    const Menu& menu = menus_[index_];
    if (row < 0 || row >= static_cast<int>(menu.items.size()) || !enabled_[static_cast<size_t>(row)]) {
        return;
    }
    MenuItem item = menu.items[static_cast<size_t>(row)];
    close();
    if (host_.showing && !atPoint_) {
        host_.showing(-1);
    }
    if (host_.run) {
        host_.run(item);
    }
}

void MenuOverlay::keyDown(const KeyPress& key) {
    int count = static_cast<int>(menus_.size());
    if (key.key == "escape" || (key.key == "f10" && key.plain()) || (key.alt && key.key.empty())) {
        close();
        if (host_.showing && !atPoint_) {
            host_.showing(-1);
        }
        return;
    }
    if (key.key == "left" && !atPoint_) {
        show((index_ + count - 1) % count);
        return;
    }
    if (key.key == "right" && !atPoint_) {
        show((index_ + 1) % count);
        return;
    }
    if (key.key == "up") {
        moveHighlight(-1);
        return;
    }
    if (key.key == "down") {
        moveHighlight(1);
        return;
    }
    if (key.key == "home") {
        highlighted_ = -1;
        moveHighlight(1);
        return;
    }
    if (key.key == "end") {
        highlighted_ = 0;
        moveHighlight(-1);
        return;
    }
    if (key.key == "enter" || key.key == "space") {
        choose(highlighted_);
        return;
    }
    // Alt+letter while a menu is open goes to that menu, as the menu bar's own would.
    if (key.alt && key.key.size() == 1 && !atPoint_) {
        for (int index = 0; index < count; ++index) {
            int hotkey = -1;
            std::string title = ui::withoutHotkeyMark(menus_[static_cast<size_t>(index)].title, &hotkey);
            if (hotkey >= 0 && hotkeyLetter(title, hotkey) == key.key) {
                show(index);
                return;
            }
        }
    }
}

void MenuOverlay::textInput(const std::string& text) {
    // A hotkey letter runs its item, as in the old menus.
    std::string typed = text;
    for (char& c : typed) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const Menu& menu = menus_[index_];
    for (size_t index = 0; index < menu.items.size(); ++index) {
        const MenuItem& item = menu.items[index];
        if (item.isSeparator || !enabled_[index]) {
            continue;
        }
        int hotkey = -1;
        std::string title = ui::withoutHotkeyMark(item.title, &hotkey);
        if (hotkey >= 0 && hotkeyLetter(title, hotkey) == typed) {
            choose(static_cast<int>(index));
            return;
        }
    }
}

bool MenuOverlay::mouseDown(const MouseEvent& event) {
    int title = titleAt(event.x, event.y);
    if (title >= 0) {
        if (title == index_) {
            close();
            if (host_.showing) {
                host_.showing(-1);
            }
        } else {
            show(title);
        }
        return true;
    }
    int row = rowAt(event.x, event.y);
    if (row >= 0) {
        choose(row);
        return true;
    }
    close();
    if (host_.showing && !atPoint_) {
        host_.showing(-1);
    }
    // A click elsewhere only closes the menu.
    return true;
}

void MenuOverlay::mouseMoved(const MouseEvent& event) {
    int title = titleAt(event.x, event.y);
    if (title >= 0 && title != index_) {
        show(title);
        return;
    }
    int row = rowAt(event.x, event.y);
    if (row >= 0 && row != highlighted_ && enabled_[static_cast<size_t>(row)]) {
        highlighted_ = row;
    }
    if (row >= 0) {
        releaseChooses_ = true;
    }
}

void MenuOverlay::mouseUp(const MouseEvent& event) {
    // Pressing on a title, sliding to an item and letting go chooses it.
    if (!releaseChooses_) {
        return;
    }
    int row = rowAt(event.x, event.y);
    if (row >= 0) {
        choose(row);
    }
}

// ---------------------------------------------------------------------------
// Dialogs

DialogOverlay::DialogOverlay(Options options, std::function<void(int, const std::string&)> done)
    : options_(std::move(options)), done_(std::move(done)) {
    for (const std::string& button : options_.buttons) {
        int hotkey = -1;
        buttonTitles_.push_back(ui::withoutHotkeyMark(button, &hotkey));
        buttonHotkeys_.push_back(hotkey);
    }
    if (options_.hasField) {
        field_.setText(options_.fieldText, options_.selectField);
    }
}

void DialogOverlay::layout(const ui::Rect& window) {
    window_ = window;
    int cell = Cell(), line = Line();
    int maxColumns = std::max(20, std::min(72, window.w / cell - 10));
    wrapped_.clear();
    for (const std::string& text : options_.lines) {
        if (options_.monospaced) {
            wrapped_.push_back(text);
            continue;
        }
        for (const std::string& piece : ui::wrapText(text, static_cast<size_t>(maxColumns))) {
            wrapped_.push_back(piece);
        }
    }
    int width = ui::textWidth(Chrome(), options_.title);
    for (const std::string& text : wrapped_) {
        width = std::max(width, ui::textWidth(Chrome(), text));
    }
    int buttons = 0;
    for (const std::string& title : buttonTitles_) {
        buttons += std::max(8 * cell, ui::textWidth(Chrome(), title) + 2 * cell) + 2 * cell;
    }
    width = std::max({width, buttons, options_.hasField ? 36 * cell : 0});
    width += 6 * cell;
    int rows = static_cast<int>(wrapped_.size()) + 4 + (options_.hasField ? 2 : 0);
    int height = line * rows;
    int x = window.x + (window.w - width) / 2;
    int y = window.y + std::max(0, (window.h - height) / 2);
    int pixel = Pixel();
    box_ = {x / pixel * pixel, y / pixel * pixel, width, height};
}

ui::Rect DialogOverlay::buttonRect(size_t index) const {
    int cell = Cell(), line = Line(), pixel = Pixel();
    std::vector<int> widths;
    int total = 0;
    for (const std::string& title : buttonTitles_) {
        widths.push_back(std::max(8 * cell, ui::textWidth(Chrome(), title) + 2 * cell));
        total += widths.back();
    }
    total += 2 * cell * (static_cast<int>(widths.size()) - 1);
    int x = box_.x + (box_.w - total) / 2;
    for (size_t step = 0; step < index; ++step) {
        x += widths[step] + 2 * cell;
    }
    return {x, box_.bottom() - line - 4 * pixel, widths[index], line - 2 * pixel};
}

void DialogOverlay::draw(ui::Canvas& canvas) {
    const bitmap::Scheme& colors = Colors();
    int pixel = Pixel(), cell = Cell(), line = Line();
    ui::drawRetroBox(canvas, box_, colors.bar, colors.barText, pixel, Shadow());
    // A title strip across the top, like the dialogs of the old IDEs.
    ui::Rect strip{box_.x, box_.y, box_.w, line};
    canvas.fill(strip, colors.dialogTitle);
    int titleWidth = ui::textWidth(Chrome(), options_.title);
    ui::drawTextPlain(canvas, Chrome(), strip.x + (strip.w - titleWidth) / 2, strip.y, options_.title, colors.dialogTitleText);
    int y = strip.bottom() + line / 2;
    for (const std::string& text : wrapped_) {
        ui::drawTextPlain(canvas, Chrome(), box_.x + 3 * cell, y, text, colors.barText);
        y += line;
    }
    if (options_.hasField) {
        y += line / 2;
        ui::Rect field{box_.x + 3 * cell, y, box_.w - 6 * cell, line};
        canvas.fill(field, colors.input);
        field_.draw(canvas, Chrome(), field.x + cell / 2, field.y, field.w - cell, colors.inputText, colors.barHighlight,
                    colors.barHighlightText, colors.inputText, true, false, options_.placeholder, colors.barDim);
    }
    for (size_t index = 0; index < buttonTitles_.size(); ++index) {
        ui::Rect button = buttonRect(index);
        canvas.fill(button.offset(2 * pixel, 2 * pixel), ui::kBlackColor);
        bool focused = static_cast<int>(index) == focusedButton_;
        canvas.fill(button, focused ? colors.dialogTitle : colors.bar);
        if (!focused) {
            canvas.frame(button, colors.barText, pixel);
        }
        int width = ui::textWidth(Chrome(), buttonTitles_[index]);
        ui::drawWithHotkey(canvas, Chrome(), button.x + (button.w - width) / 2, button.y - pixel, buttonTitles_[index],
                           buttonHotkeys_[index], colors.dialogTitleText, colors.barHotkey, false);
    }
}

void DialogOverlay::finish(int button) {
    close();
    if (done_) {
        auto done = done_;
        std::string text = field_.text();
        done(button, text);
    }
}

void DialogOverlay::keyDown(const KeyPress& key) {
    if (key.key == "escape") {
        finish(options_.cancelButton >= 0 ? options_.cancelButton
                                          : (buttonTitles_.size() > 1 ? static_cast<int>(buttonTitles_.size()) - 1 : -1));
        return;
    }
    if (key.key == "enter") {
        finish(focusedButton_);
        return;
    }
    if (key.key == "tab" || ((key.key == "left" || key.key == "right") && !options_.hasField)) {
        int count = static_cast<int>(buttonTitles_.size());
        int step = (key.shift || key.key == "left") ? -1 : 1;
        if (count > 0) {
            focusedButton_ = (focusedButton_ + step + count) % count;
        }
        return;
    }
    if (options_.hasField) {
        field_.keyDown(key);
        return;
    }
    // Alt+hotkey (or the letter alone, without a field) presses a button.
    if (key.key.size() == 1) {
        for (size_t index = 0; index < buttonTitles_.size(); ++index) {
            if (buttonHotkeys_[index] >= 0 && hotkeyLetter(buttonTitles_[index], buttonHotkeys_[index]) == key.key &&
                (key.alt || !options_.hasField)) {
                finish(static_cast<int>(index));
                return;
            }
        }
    }
}

void DialogOverlay::textInput(const std::string& text) {
    if (options_.hasField) {
        field_.insert(text);
        return;
    }
    std::string typed = text;
    for (char& c : typed) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (size_t index = 0; index < buttonTitles_.size(); ++index) {
        if (buttonHotkeys_[index] >= 0 && hotkeyLetter(buttonTitles_[index], buttonHotkeys_[index]) == typed) {
            finish(static_cast<int>(index));
            return;
        }
    }
    if (typed == " ") {
        finish(focusedButton_);
    }
}

bool DialogOverlay::mouseDown(const MouseEvent& event) {
    for (size_t index = 0; index < buttonTitles_.size(); ++index) {
        if (buttonRect(index).contains(event.x, event.y)) {
            finish(static_cast<int>(index));
            return true;
        }
    }
    if (options_.hasField) {
        int line = Line();
        int fieldTop = box_.y + line + line / 2 + static_cast<int>(wrapped_.size()) * line + line / 2;
        ui::Rect field{box_.x + 3 * Cell(), fieldTop, box_.w - 6 * Cell(), line};
        if (field.contains(event.x, event.y)) {
            field_.clickAt(event.x - field.x - Cell() / 2, Chrome().cellWidth(), event.shift);
            return true;
        }
    }
    if (!box_.contains(event.x, event.y) && buttonTitles_.size() <= 1 && !options_.hasField) {
        finish(buttonTitles_.empty() ? -1 : 0);  // a click elsewhere closes a message
    }
    return true;
}

// ---------------------------------------------------------------------------
// The palette

bool paletteMatch(const std::string& query, PaletteItem& item, int* score) {
    if (query.empty()) {
        if (score) {
            *score = 0;
        }
        item.matched.clear();
        return true;
    }
    std::optional<FuzzyMatch> match = fuzzyMatch(query, item.title);
    if (!match) {
        return false;
    }
    item.matched = match->positions;
    if (score) {
        *score = match->score;
    }
    return true;
}

PaletteOverlay::PaletteOverlay(std::string placeholder, Provider provider, std::function<void()> onDismiss)
    : placeholder_(std::move(placeholder)), provider_(std::move(provider)), onDismiss_(std::move(onDismiss)) {
    field_.onChange = [this] { reload(); };
    reload();
}

void PaletteOverlay::layout(const ui::Rect& window) {
    window_ = window;
    int pixel = Pixel(), cell = Cell(), line = Line();
    int width = std::min(window.w - 4 * cell, static_cast<int>(std::lround(640 * Services::shared().displayScale())));
    width = std::max(width, 30 * cell);
    int height = 8 * pixel + line + line / 2 + 10 * line + line / 2;
    int x = window.x + (window.w - width) / 2;
    int y = window.y + static_cast<int>(std::lround(56 * Services::shared().displayScale()));
    box_ = {x / pixel * pixel, y / pixel * pixel, width, height};
}

ui::Rect PaletteOverlay::rowRect(size_t visibleIndex) const {
    int pixel = Pixel(), line = Line();
    int top = box_.y + 4 * pixel + line + line / 2;
    return {box_.x + 4 * pixel, top + static_cast<int>(visibleIndex) * line, box_.w - 8 * pixel, line};
}

void PaletteOverlay::draw(ui::Canvas& canvas) {
    const bitmap::Scheme& colors = Colors();
    int pixel = Pixel(), cell = Cell(), line = Line();
    ui::drawRetroBox(canvas, box_, colors.bar, colors.barText, pixel, Shadow());
    // The query.
    int fieldY = box_.y + 4 * pixel;
    field_.draw(canvas, Chrome(), box_.x + 2 * cell, fieldY, box_.w - 4 * cell, colors.barText, colors.barHighlight,
                colors.barHighlightText, colors.barText, true, false, placeholder_, colors.barDim);
    canvas.fill({box_.x + 2 * pixel, fieldY + line + line / 4, box_.w - 4 * pixel, pixel}, colors.barText);
    // The rows.
    for (size_t visible = 0; visible < 10 && scroll_ + visible < items_.size(); ++visible) {
        size_t index = scroll_ + visible;
        const PaletteItem& item = items_[index];
        ui::Rect row = rowRect(visible);
        bool selected = index == selected_;
        if (selected) {
            canvas.fill(row, colors.barHighlight);
        }
        Color ink = selected ? colors.barHighlightText : colors.barText;
        Color dim = selected ? colors.barHighlightText : colors.barDim;
        int shortcutWidth = item.shortcut.empty() ? 0 : ui::textWidth(Chrome(), item.shortcut) + cell;
        canvas.pushClip({row.x, row.y, row.w - shortcutWidth - cell, row.h});
        int x = row.x + cell;
        size_t match = 0;
        for (size_t offset = 0; offset < item.title.size();) {
            size_t at = offset;
            char32_t character = ui::decodeUtf8(item.title, offset);
            while (match < item.matched.size() && item.matched[match] < at) {
                ++match;
            }
            bool matched = match < item.matched.size() && item.matched[match] == at;
            ui::drawGlyphPlain(canvas, Chrome(), character, x, row.y, matched ? colors.barHotkey : ink);
            x += ui::Font::cells(character) * cell;
        }
        if (!item.subtitle.empty()) {
            ui::drawTextPlain(canvas, Chrome(), x + 2 * cell, row.y, item.subtitle, dim);
        }
        canvas.popClip();
        if (!item.shortcut.empty()) {
            ui::drawTextPlain(canvas, Chrome(), row.right() - shortcutWidth, row.y, item.shortcut, dim);
        }
    }
    if (items_.empty()) {
        ui::drawTextPlain(canvas, Chrome(), rowRect(0).x + cell, rowRect(0).y, "Nothing matches.", colors.barDim);
    }
    // A little scrollbar when the list is longer than the box.
    if (items_.size() > 10) {
        ui::Rect first = rowRect(0);
        int trackHeight = 10 * line;
        int thumb = std::max(line / 2, trackHeight * 10 / static_cast<int>(items_.size()));
        int top = first.y + static_cast<int>((trackHeight - thumb) * scroll_ / std::max<size_t>(1, items_.size() - 10));
        canvas.fill({box_.right() - 4 * pixel - 2 * pixel, top, 2 * pixel, thumb}, colors.barDim);
    }
}

void PaletteOverlay::reload() {
    items_ = provider_ ? provider_(field_.text()) : std::vector<PaletteItem>();
    selected_ = 0;
    scroll_ = 0;
}

void PaletteOverlay::refresh() {
    size_t keep = selected_;
    items_ = provider_ ? provider_(field_.text()) : std::vector<PaletteItem>();
    selected_ = std::min(keep, items_.empty() ? 0 : items_.size() - 1);
    if (selected_ < scroll_ || selected_ >= scroll_ + 10) {
        scroll_ = selected_ >= 9 ? selected_ - 9 : 0;
    }
}

void PaletteOverlay::setQuery(const std::string& query) {
    field_.setText(query);
    reload();
}

void PaletteOverlay::run(size_t index) {
    if (index >= items_.size()) {
        return;
    }
    ran_ = true;
    std::function<void()> work = items_[index].run;
    close();
    if (work) {
        work();
    }
}

void PaletteOverlay::dismiss() {
    close();
    if (onDismiss_ && !ran_) {
        onDismiss_();
    }
}

void PaletteOverlay::keyDown(const KeyPress& key) {
    auto select = [&](long index) {
        if (items_.empty()) {
            return;
        }
        selected_ = static_cast<size_t>(std::clamp(index, 0L, static_cast<long>(items_.size()) - 1));
        if (selected_ < scroll_) {
            scroll_ = selected_;
        } else if (selected_ >= scroll_ + 10) {
            scroll_ = selected_ - 9;
        }
    };
    if (key.key == "escape") {
        dismiss();
    } else if (key.key == "enter") {
        run(selected_);
    } else if (key.key == "up" || (key.control && key.key == "p")) {
        select(static_cast<long>(selected_) - 1);
    } else if (key.key == "down" || (key.control && key.key == "n")) {
        select(static_cast<long>(selected_) + 1);
    } else if (key.key == "pageup") {
        select(static_cast<long>(selected_) - 10);
    } else if (key.key == "pagedown") {
        select(static_cast<long>(selected_) + 10);
    } else {
        field_.keyDown(key);
    }
}

void PaletteOverlay::textInput(const std::string& text) {
    field_.insert(text);
}

bool PaletteOverlay::mouseDown(const MouseEvent& event) {
    if (!box_.contains(event.x, event.y)) {
        dismiss();
        return true;
    }
    for (size_t visible = 0; visible < 10 && scroll_ + visible < items_.size(); ++visible) {
        if (rowRect(visible).contains(event.x, event.y)) {
            selected_ = scroll_ + visible;
            run(selected_);
            return true;
        }
    }
    return true;
}

void PaletteOverlay::mouseMoved(const MouseEvent& event) {
    for (size_t visible = 0; visible < 10 && scroll_ + visible < items_.size(); ++visible) {
        if (rowRect(visible).contains(event.x, event.y)) {
            selected_ = scroll_ + visible;
        }
    }
}

void PaletteOverlay::wheel(const WheelEvent& event) {
    if (items_.size() <= 10) {
        return;
    }
    long next = static_cast<long>(scroll_) - static_cast<long>(std::lround(event.dy * 3));
    scroll_ = static_cast<size_t>(std::clamp(next, 0L, static_cast<long>(items_.size()) - 10));
}

}  // namespace gw::app
