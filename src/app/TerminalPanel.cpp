#include "TerminalPanel.hpp"

#include "Loop.hpp"
#include "Paths.hpp"
#include "PixelArt.hpp"
#include "Platform.hpp"
#include "Services.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <regex>

namespace gw::app {

namespace {

/// Output fed to the screen per main-thread turn, so a flood can't freeze the window.
constexpr size_t kSliceBytes = 256 * 1024;

int Pixel() { return Services::shared().chromePixel(); }
int Cell() { return 6 * Pixel(); }
int Line() { return 12 * Pixel(); }
ui::Font& Chrome() { return Services::shared().chromeFont(); }

}  // namespace

// ---------------------------------------------------------------------------
// Sessions

TerminalSession::TerminalSession(const std::string& folder, const std::string& command, int columns, int rows) : folder_(folder) {
    screen_ = std::make_unique<term::Screen>(std::max(2, columns), std::max(2, rows));
    pty_ = std::make_unique<Pty>();
    screen_->onReply = [this](const std::string& reply) { send(reply); };
    std::weak_ptr<bool> alive = alive_;
    pty_->onOutput = [this, alive](std::string_view bytes) {
        if (alive.expired()) {
            return;
        }
        received(bytes);
    };
    pty_->onExit = [this, alive](int code) {
        runOnMain([this, alive, code] {
            if (alive.expired()) {
                return;
            }
            exited_ = true;
            if (onExit) {
                onExit(code);
            }
        });
    };
    PtyOptions options;
    options.workingDirectory = folder;
    options.columns = std::max(2, columns);
    options.rows = std::max(2, rows);
    const std::string& configured = Services::shared().settings().terminalShell;
    if (!configured.empty()) {
        options.program = configured;
    }
    if (!pty_->start(options, &error_)) {
        screen_->write("\x1b[31mCouldn't start the shell: " + error_ + "\x1b[0m\r\n");
        exited_ = true;
    }
    std::string program = options.program.empty() ? defaultShell() : options.program;
    size_t slash = program.find_last_of("\\/");
    shellName_ = slash == std::string::npos ? program : program.substr(slash + 1);
    if (shellName_.size() > 4 && shellName_.compare(shellName_.size() - 4, 4, ".exe") == 0) {
        shellName_.resize(shellName_.size() - 4);
    }
    if (!command.empty() && !exited_) {
        send(command + "\r");
    }
}

TerminalSession::~TerminalSession() {
    alive_.reset();
    pty_->onOutput = nullptr;
    pty_->onExit = nullptr;
    pty_.reset();
}

void TerminalSession::received(std::string_view bytes) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.append(bytes);
        if (!feeding_) {
            feeding_ = true;
            start = true;
        }
    }
    if (start) {
        std::weak_ptr<bool> alive = alive_;
        runOnMain([this, alive] {
            if (!alive.expired()) {
                feedSlice();
            }
        });
    }
}

void TerminalSession::feedSlice() {
    std::string slice;
    bool more = false;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pending_.size() <= kSliceBytes) {
            slice.swap(pending_);
        } else {
            slice = pending_.substr(0, kSliceBytes);
            pending_.erase(0, kSliceBytes);
            more = true;
        }
        feeding_ = more;
    }
    screen_->write(slice);
    pty_->acknowledge(slice.size());
    if (onChange) {
        onChange();
    }
    if (more) {
        std::weak_ptr<bool> alive = alive_;
        afterDelay(0.008, [this, alive] {
            if (!alive.expired()) {
                feedSlice();
            }
        });
    }
}

void TerminalSession::send(const std::string& bytes) {
    if (!exited_) {
        pty_->write(bytes);
    }
}

void TerminalSession::paste(const std::string& text) {
    std::string cleaned;
    for (size_t index = 0; index < text.size(); ++index) {
        // Line breaks are typed as Return.
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
            continue;
        }
        cleaned += text[index] == '\n' ? '\r' : text[index];
    }
    if (screen_->bracketedPaste()) {
        send("\x1b[200~" + cleaned + "\x1b[201~");
    } else {
        send(cleaned);
    }
}

void TerminalSession::resize(int columns, int rows) {
    columns = std::max(2, columns);
    rows = std::max(2, rows);
    if (columns == screen_->columns() && rows == screen_->rows()) {
        return;
    }
    screen_->resize(columns, rows);
    pty_->resize(columns, rows);
}

void TerminalSession::terminate() {
    pty_->terminate();
}

bool TerminalSession::isRunning() const {
    return !exited_ && pty_->isRunning();
}

std::string TerminalSession::title() const {
    std::string title = screen_->title();
    // Windows consoles title themselves with the program's full path: show its name.
    std::string lower = title;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) {
        size_t slash = title.find_last_of("\\/");
        title = title.substr(slash == std::string::npos ? 0 : slash + 1);
        title = title.substr(0, title.size() - 4);
    }
    if (!title.empty()) {
        return title;
    }
    std::string front = pty_->foregroundProcessName();
    return front.empty() ? shellName_ : front;
}

bool TerminalSession::isIdleAtPrompt() const {
    if (!isRunning()) {
        return false;
    }
    std::string front = pty_->foregroundProcessName();
    if (front.empty()) {
        return true;
    }
    std::string lower = front;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string shell = shellName_;
    std::transform(shell.begin(), shell.end(), shell.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == shell || lower == "bash" || lower == "zsh" || lower == "fish" || lower == "sh" || lower == "pwsh" ||
           lower == "powershell" || lower == "cmd";
}

// ---------------------------------------------------------------------------
// The panel

TerminalPanel::TerminalPanel() {
    reloadAppearance();
}

TerminalPanel::~TerminalPanel() {
    for (auto& session : sessions_) {
        session->terminate();
    }
}

TerminalSession* TerminalPanel::current() const {
    return selected_ < sessions_.size() ? sessions_[selected_].get() : nullptr;
}

void TerminalPanel::reloadAppearance() {
    Services& services = Services::shared();
    font_ = services.terminalFont();
    static const uint32_t fallback[16] = {0x000000FF, 0xCD3131FF, 0x0DBC79FF, 0xE5E510FF, 0x2472C8FF, 0xBC3FBCFF,
                                          0x11A8CDFF, 0xE5E5E5FF, 0x666666FF, 0xF14C4CFF, 0x23D18BFF, 0xF5F543FF,
                                          0x3B8EEAFF, 0xD670D6FF, 0x29B8DBFF, 0xFFFFFFFF};
    static const char* names[16] = {"Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White",
                                    "BrightBlack", "BrightRed", "BrightGreen", "BrightYellow", "BrightBlue",
                                    "BrightMagenta", "BrightCyan", "BrightWhite"};
    for (int index = 0; index < 16; ++index) {
        palette_[index] = services.color(std::string("terminal.ansi") + names[index], fallback[index]);
    }
    for (int index = 16; index < 232; ++index) {
        int value = index - 16;
        auto level = [](int step) -> uint32_t { return step == 0 ? 0 : static_cast<uint32_t>(55 + step * 40); };
        palette_[index] = (level(value / 36) << 24) | (level((value / 6) % 6) << 16) | (level(value % 6) << 8) | 0xFF;
    }
    for (int index = 232; index < 256; ++index) {
        uint32_t grey = static_cast<uint32_t>(8 + (index - 232) * 10);
        palette_[index] = (grey << 24) | (grey << 16) | (grey << 8) | 0xFF;
    }
    foreground_ = services.color("terminal.foreground", services.color("editor.foreground", 0xD4D4D4FF));
    background_ = (services.color("terminal.background", services.color("editor.background", 0x000000FF)) & 0xFFFFFF00u) | 0xFF;
    cursorColor_ = services.color("terminalCursor.foreground", services.color("editorCursor.foreground", 0xFFFFFFFF));
    selectionColor_ = services.color("terminal.selectionBackground", services.color("editor.selectionBackground", 0x00AAAAFF));
    auto toTerm = [](Color color) {
        return term::TermColor::rgb(static_cast<uint8_t>(color >> 24), static_cast<uint8_t>(color >> 16), static_cast<uint8_t>(color >> 8));
    };
    for (auto& session : sessions_) {
        session->screen().setDefaultColors(toTerm(foreground_), toTerm(background_));
    }
    updateGridSize();
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
}

ui::Rect TerminalPanel::stripRect() const {
    return {frame_.x, frame_.y, frame_.w, Line()};
}

ui::Rect TerminalPanel::screenRect() const {
    int strip = Line() + Pixel() * 2;
    return {frame_.x, frame_.y + strip, frame_.w, std::max(0, frame_.h - strip)};
}

void TerminalPanel::setFrame(const ui::Rect& frame) {
    if (frame == frame_) {
        return;
    }
    frame_ = frame;
    updateGridSize();
}

void TerminalPanel::updateGridSize() {
    ui::Rect area = screenRect();
    int columns = std::max(2, area.w / std::max(1, font_->cellWidth()));
    int rows = std::max(2, area.h / std::max(1, font_->lineHeight()));
    gridColumns_ = columns;
    gridRows_ = rows;
    for (auto& session : sessions_) {
        session->resize(columns, rows);
    }
}

void TerminalPanel::newTerminal(const std::string& command) {
    updateGridSize();
    std::string folder = projectFolder.empty() ? homeFolder() : projectFolder;
    auto session = std::make_unique<TerminalSession>(folder, command, std::max(gridColumns_, 20), std::max(gridRows_, 5));
    auto toTerm = [](Color color) {
        return term::TermColor::rgb(static_cast<uint8_t>(color >> 24), static_cast<uint8_t>(color >> 16), static_cast<uint8_t>(color >> 8));
    };
    session->screen().setDefaultColors(toTerm(foreground_), toTerm(background_));
    TerminalSession* raw = session.get();
    session->onChange = [this] {
        if (onNeedsDisplay) {
            onNeedsDisplay();
        }
    };
    session->onExit = [this, raw](int) {
        // The shell ended: its tab goes.
        for (size_t index = 0; index < sessions_.size(); ++index) {
            if (sessions_[index].get() == raw) {
                if (taskSession_ == static_cast<long>(index)) {
                    taskSession_ = -1;
                } else if (taskSession_ > static_cast<long>(index)) {
                    --taskSession_;
                }
                runOnMain([this, raw] {
                    for (size_t position = 0; position < sessions_.size(); ++position) {
                        if (sessions_[position].get() == raw) {
                            sessions_.erase(sessions_.begin() + static_cast<long>(position));
                            if (selected_ >= sessions_.size() && !sessions_.empty()) {
                                selected_ = sessions_.size() - 1;
                            }
                            if (sessions_.empty() && onHide) {
                                onHide();
                            }
                            if (onNeedsDisplay) {
                                onNeedsDisplay();
                            }
                            return;
                        }
                    }
                });
                return;
            }
        }
    };
    sessions_.push_back(std::move(session));
    select(sessions_.size() - 1);
}

void TerminalPanel::runTaskCommand(const std::string& command) {
    if (taskSession_ >= 0 && static_cast<size_t>(taskSession_) < sessions_.size() &&
        sessions_[static_cast<size_t>(taskSession_)]->isIdleAtPrompt()) {
        select(static_cast<size_t>(taskSession_));
        current()->send(command + "\r");
        return;
    }
    newTerminal(command);
    taskSession_ = static_cast<long>(sessions_.size()) - 1;
}

void TerminalPanel::select(size_t index) {
    if (index < sessions_.size()) {
        selected_ = index;
        scrollOffset_ = 0;
        hasSelection_ = false;
    }
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
}

void TerminalPanel::closeCurrentTerminal() {
    if (TerminalSession* session = current()) {
        session->terminate();
    }
}

void TerminalPanel::clear() {
    if (TerminalSession* session = current()) {
        // Erase the screen and the history, then ask the shell for a fresh prompt.
        session->screen().write("\x1b[H\x1b[2J\x1b[3J");
        session->send("\x0c");
        scrollOffset_ = 0;
        hasSelection_ = false;
    }
}

void TerminalPanel::sendText(const std::string& text) {
    if (TerminalSession* session = current()) {
        session->send(text);
    }
}

void TerminalPanel::focusChanged(bool focused) {
    focused_ = focused;
}

// ---------------------------------------------------------------------------
// Drawing

Color TerminalPanel::colorFor(term::TermColor color, bool foreground, bool bold) const {
    if (color.isIndexed()) {
        uint8_t index = color.index();
        if (foreground && bold && index < 8) {
            index += 8;
        }
        return palette_[index];
    }
    if (color.isRgb()) {
        return (static_cast<uint32_t>(color.red()) << 24) | (static_cast<uint32_t>(color.green()) << 16) |
               (static_cast<uint32_t>(color.blue()) << 8) | 0xFF;
    }
    return foreground ? foreground_ : 0;
}

bool TerminalPanel::selectionContains(long row, int column) const {
    if (!hasSelection_) {
        return false;
    }
    Point start = anchor_, end = head_;
    auto before = [](Point a, Point b) { return a.row < b.row || (a.row == b.row && a.column < b.column); };
    if (before(end, start)) {
        std::swap(start, end);
    }
    Point here{row, column};
    return !before(here, start) && before(here, {end.row, end.column + 1});
}

void TerminalPanel::draw(ui::Canvas& canvas, bool focused) {
    focused_ = focused;
    const bitmap::Scheme& colors = Services::shared().scheme();
    int pixel = Pixel(), cell = Cell();
    canvas.pushClip(frame_);
    canvas.fill(frame_, colors.window);
    // The tab strip: a tab per terminal, then [+] [x] [v] at the right.
    stripButtons_.clear();
    ui::Rect strip = stripRect();
    int x = strip.x;
    for (size_t index = 0; index < sessions_.size(); ++index) {
        std::string label = " " + std::to_string(index + 1) + ": " + sessions_[index]->title() + " ";
        if (ui::textCells(label) > 24) {
            label = label.substr(0, 22) + "… ";
        }
        int width = ui::textWidth(Chrome(), label);
        bool selected = index == selected_;
        if (selected) {
            canvas.fill({x, strip.y, width, strip.h}, colors.selection);
        }
        if (selected) {
            ui::drawTextPlain(canvas, Chrome(), x, strip.y, label, colors.selectionText);
        } else {
            ui::drawText(canvas, Chrome(), x, strip.y, label, colors.text, true);
        }
        stripButtons_.push_back({{x, strip.y, width, strip.h}, static_cast<int>(index)});
        x += width + cell / 2;
    }
    const char* labels[3] = {"[+]", "[x]", "[v]"};
    int right = strip.right();
    for (int button = 2; button >= 0; --button) {
        int width = ui::textWidth(Chrome(), labels[button]);
        right -= width + (button < 2 ? cell / 2 : 0);
        ui::drawText(canvas, Chrome(), right, strip.y, labels[button], colors.text, true);
        stripButtons_.push_back({{right, strip.y, width, strip.h}, -(button + 1)});
    }
    canvas.fillShadowed({strip.x, strip.bottom(), strip.w, pixel}, colors.frameDim, std::max(1, font_->pixel()));

    TerminalSession* session = current();
    ui::Rect area = screenRect();
    canvas.fill(area, background_);
    if (session == nullptr) {
        ui::drawText(canvas, Chrome(), area.x + cell, area.y + pixel, "No terminal. [+] opens one.", colors.textDim, true);
        canvas.popClip();
        return;
    }
    canvas.pushClip(area);
    term::Screen& screen = session->screen();
    ui::Font& font = *font_;
    int cellWidth = font.cellWidth(), cellHeight = font.lineHeight();
    int rows = screen.rows();
    int columns = screen.columns();
    for (long line = 0; line < rows; ++line) {
        long rowIndex = line - scrollOffset_;
        const term::Row& row = screen.row(rowIndex);
        int y = area.y + static_cast<int>(line) * cellHeight;
        // Backgrounds and the selection.
        for (int column = 0; column < columns; ++column) {
            term::Cell cellValue = column < static_cast<int>(row.cells.size()) ? row.cells[static_cast<size_t>(column)] : term::Cell();
            bool inverse = (cellValue.attributes & term::kInverse) != 0;
            term::TermColor backgroundColor = inverse ? cellValue.foreground : cellValue.background;
            Color fill = 0;
            if (inverse) {
                fill = backgroundColor.isDefault() ? foreground_ : colorFor(backgroundColor, true, false);
            } else if (!backgroundColor.isDefault()) {
                fill = colorFor(backgroundColor, false, false);
            }
            if (selectionContains(rowIndex, column)) {
                fill = selectionColor_;
            }
            if (fill != 0) {
                canvas.fill({area.x + column * cellWidth, y, cellWidth, cellHeight}, fill);
            }
        }
        // Characters.
        for (int column = 0; column < columns && column < static_cast<int>(row.cells.size()); ++column) {
            const term::Cell& cellValue = row.cells[static_cast<size_t>(column)];
            if (cellValue.attributes & term::kWideEnd) {
                continue;
            }
            bool bold = (cellValue.attributes & term::kBold) != 0;
            bool inverse = (cellValue.attributes & term::kInverse) != 0;
            term::TermColor ink = inverse ? cellValue.background : cellValue.foreground;
            Color color = inverse ? (ink.isDefault() ? background_ : colorFor(ink, false, false)) : colorFor(ink, true, bold);
            if (cellValue.attributes & term::kDim) {
                color = ui::mix(color, background_, 0.4);
            }
            if (cellValue.attributes & term::kHidden) {
                continue;
            }
            int cellX = area.x + column * cellWidth;
            if (cellValue.character > ' ') {
                ui::drawGlyph(canvas, font, static_cast<char32_t>(cellValue.character), cellX, y, color, !inverse && font.isPixelFont());
            }
            int width = (cellValue.attributes & term::kWideStart) ? 2 : 1;
            if (cellValue.attributes & term::kUnderline) {
                canvas.fillShadowed({cellX, y + font.baseline() + font.pixel(), width * cellWidth, std::max(1, font.pixel())}, color, std::max(1, font_->pixel()));
            }
            if (cellValue.attributes & term::kStrike) {
                canvas.fillShadowed({cellX, y + cellHeight / 2, width * cellWidth, std::max(1, font.pixel())}, color, std::max(1, font_->pixel()));
            }
        }
    }
    // The cursor: a block when focused (the character under it in the background color), an
    // outline otherwise.
    if (scrollOffset_ == 0 && screen.cursorVisible()) {
        int cursorColumn = screen.cursorColumn(), cursorRow = screen.cursorRow();
        ui::Rect rect{area.x + cursorColumn * cellWidth, area.y + cursorRow * cellHeight, cellWidth, cellHeight};
        if (focused_) {
            canvas.fillShadowed(rect, ui::kWhiteColor, std::max(1, font.pixel()));
            term::Cell under = screen.cellAt(cursorRow, cursorColumn);
            if (under.character > ' ') {
                ui::drawGlyphPlain(canvas, font, static_cast<char32_t>(under.character), rect.x, rect.y, background_);
            }
        } else {
            int line = std::max(1, font.pixel() / 2);
            canvas.frame(rect.offset(line, line), ui::kBlackColor, line);
            canvas.frame(rect, ui::kWhiteColor, line);
        }
    }
    // A slim indicator while looking at the history.
    if (scrollOffset_ > 0 && screen.historyCount() > 0) {
        double total = static_cast<double>(screen.historyCount() + rows);
        int trackHeight = area.h;
        int thumbHeight = std::max(Line(), static_cast<int>(trackHeight * rows / total));
        int thumbTop = area.y + static_cast<int>((trackHeight - thumbHeight) * (1.0 - static_cast<double>(scrollOffset_) / screen.historyCount()));
        canvas.fillShadowed({area.right() - 2 * pixel, thumbTop, 2 * pixel, thumbHeight}, selectionColor_, std::max(1, font_->pixel()));
    }
    canvas.popClip();
    canvas.popClip();
}

// ---------------------------------------------------------------------------
// Keyboard

std::string TerminalPanel::sequenceForKey(const KeyPress& key) const {
    TerminalSession* session = current();
    bool applicationKeys = session && session->screen().applicationCursorKeys();
    int modifier = 1 + (key.shift ? 1 : 0) + (key.alt ? 2 : 0) + (key.control ? 4 : 0);
    auto arrow = [&](char letter) {
        if (modifier > 1) {
            return "\x1b[1;" + std::to_string(modifier) + letter;
        }
        return std::string(applicationKeys ? "\x1bO" : "\x1b[") + letter;
    };
    auto tilde = [&](int number) {
        if (modifier > 1) {
            return "\x1b[" + std::to_string(number) + ";" + std::to_string(modifier) + "~";
        }
        return "\x1b[" + std::to_string(number) + "~";
    };
    const std::string& name = key.key;
    if (name == "up") return arrow('A');
    if (name == "down") return arrow('B');
    if (name == "right") return arrow('C');
    if (name == "left") return arrow('D');
    if (name == "home") return arrow('H');
    if (name == "end") return arrow('F');
    if (name == "pageup") return tilde(5);
    if (name == "pagedown") return tilde(6);
    if (name == "delete") return tilde(3);
    if (name == "insert") return tilde(2);
    if (name == "backspace") return key.control ? std::string("\x08") : key.alt ? std::string("\x1b\x7f") : std::string("\x7f");
    if (name == "enter") return key.alt ? std::string("\x1b\r") : std::string("\r");
    if (name == "tab") return key.shift ? std::string("\x1b[Z") : std::string("\t");
    if (name == "escape") return "\x1b";
    if (name == "f1") return modifier > 1 ? "\x1b[1;" + std::to_string(modifier) + "P" : std::string("\x1bOP");
    if (name == "f2") return modifier > 1 ? "\x1b[1;" + std::to_string(modifier) + "Q" : std::string("\x1bOQ");
    if (name == "f3") return modifier > 1 ? "\x1b[1;" + std::to_string(modifier) + "R" : std::string("\x1bOR");
    if (name == "f4") return modifier > 1 ? "\x1b[1;" + std::to_string(modifier) + "S" : std::string("\x1bOS");
    static const std::pair<const char*, int> functionKeys[] = {{"f5", 15}, {"f6", 17}, {"f7", 18}, {"f8", 19},
                                                               {"f9", 20}, {"f10", 21}, {"f11", 23}, {"f12", 24}};
    for (const auto& [function, number] : functionKeys) {
        if (name == function) {
            return tilde(number);
        }
    }
    // Control with a letter or symbol sends the matching control character.
    if (key.control && !key.meta && name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') {
            return std::string(key.alt ? "\x1b" : "") + std::string(1, static_cast<char>(c - 'a' + 1));
        }
        switch (c) {
            case ' ': case '2': return std::string(1, '\0');
            case '[': case '3': return "\x1b";
            case '\\': case '4': return "\x1c";
            case ']': case '5': return "\x1d";
            case '6': return "\x1e";
            case '/': case '7': return "\x1f";
            case '8': return "\x7f";
            default: break;
        }
    }
    if (name == "space" && key.control) {
        return std::string(1, '\0');
    }
    // Alt with a character: Escape first, as terminals send Meta.
    if (key.alt && !key.control && name.size() == 1) {
        char c = name[0];
        if (key.shift && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        return std::string("\x1b") + c;
    }
    return std::string();
}

bool TerminalPanel::keyDown(const KeyPress& key) {
    TerminalSession* session = current();
    if (session == nullptr) {
        return false;
    }
    // Copy and paste: Ctrl+C copies a selection (and is ^C without one); Ctrl+V pastes; the
    // Shift versions always copy and paste, as in Windows Terminal.
    if (key.control && !key.alt && key.key == "c" && (hasSelection_ || key.shift)) {
        copySelection();
        return true;
    }
    if (key.control && !key.alt && key.key == "v") {
        paste();
        return true;
    }
    if (key.shift && !key.control && key.key == "insert") {
        paste();
        return true;
    }
    if (key.shift && !key.control && (key.key == "pageup" || key.key == "pagedown")) {
        long rows = session->screen().rows() - 1;
        long next = scrollOffset_ + (key.key == "pageup" ? rows : -rows);
        scrollOffset_ = std::clamp<long>(next, 0, static_cast<long>(session->screen().historyCount()));
        if (onNeedsDisplay) {
            onNeedsDisplay();
        }
        return true;
    }
    std::string sequence = sequenceForKey(key);
    if (sequence.empty()) {
        return false;
    }
    scrollOffset_ = 0;
    hasSelection_ = false;
    session->send(sequence);
    return true;
}

void TerminalPanel::textInput(const std::string& text) {
    if (TerminalSession* session = current()) {
        scrollOffset_ = 0;
        hasSelection_ = false;
        session->send(text);
    }
}

void TerminalPanel::copySelection() {
    std::string text = selectedText();
    if (!text.empty()) {
        setClipboardText(text);
    }
    hasSelection_ = false;
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
}

void TerminalPanel::paste() {
    if (TerminalSession* session = current()) {
        std::string text = clipboardText();
        if (!text.empty()) {
            scrollOffset_ = 0;
            session->paste(text);
        }
    }
}

void TerminalPanel::selectAll() {
    TerminalSession* session = current();
    if (session == nullptr) {
        return;
    }
    term::Screen& screen = session->screen();
    anchor_ = {-static_cast<long>(screen.historyCount()), 0};
    head_ = {screen.rows() - 1, screen.columns() - 1};
    hasSelection_ = true;
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
}

std::string TerminalPanel::selectedText() const {
    TerminalSession* session = current();
    if (session == nullptr || !hasSelection_) {
        return std::string();
    }
    Point start = anchor_, end = head_;
    if (end.row < start.row || (end.row == start.row && end.column < start.column)) {
        std::swap(start, end);
    }
    return session->screen().textInRange(start.row, start.column, end.row, end.column + 1);
}

// ---------------------------------------------------------------------------
// Mouse

TerminalPanel::Point TerminalPanel::pointAt(int x, int y) const {
    ui::Rect area = screenRect();
    TerminalSession* session = current();
    int columns = session ? session->screen().columns() : 1;
    int rows = session ? session->screen().rows() : 1;
    int column = std::clamp((x - area.x) / std::max(1, font_->cellWidth()), 0, columns - 1);
    int line = std::clamp((y - area.y) / std::max(1, font_->lineHeight()), 0, rows - 1);
    return {line - scrollOffset_, column};
}

bool TerminalPanel::reportMouse(const MouseEvent& event, int button, bool pressed, bool motion) {
    TerminalSession* session = current();
    term::Screen& screen = session->screen();
    if (screen.mouseMode() == term::MouseMode::None || event.shift || event.alt) {
        return false;  // with Shift or Alt the mouse selects text instead
    }
    ui::Rect area = screenRect();
    int column = std::clamp((event.x - area.x) / std::max(1, font_->cellWidth()), 0, screen.columns() - 1) + 1;
    int line = std::clamp((event.y - area.y) / std::max(1, font_->lineHeight()), 0, screen.rows() - 1) + 1;
    int code = button + (motion ? 32 : 0) + (event.control ? 16 : 0);
    std::string message;
    if (screen.mouseSgr()) {
        message = "\x1b[<" + std::to_string(code) + ";" + std::to_string(column) + ";" + std::to_string(line) + (pressed ? "M" : "m");
    } else {
        int legacy = pressed ? code : 3;
        message = "\x1b[M";
        message += static_cast<char>(32 + legacy);
        message += static_cast<char>(32 + std::min(column, 223));
        message += static_cast<char>(32 + std::min(line, 223));
    }
    session->send(message);
    return true;
}

void TerminalPanel::wordRangeAt(Point point, int& start, int& end) const {
    term::Screen& screen = current()->screen();
    auto isWordCharacter = [](uint32_t c) {
        if (c == 0) {
            return false;
        }
        if (c > 0x7F) {
            return true;
        }
        return std::isalnum(static_cast<int>(c)) != 0 || std::strchr("_-./\\~:@%+=#?&,", static_cast<int>(c)) != nullptr;
    };
    int from = point.column, to = point.column;
    if (!isWordCharacter(screen.cellAt(point.row, point.column).character)) {
        start = end = point.column;
        return;
    }
    while (from > 0 && isWordCharacter(screen.cellAt(point.row, from - 1).character)) {
        --from;
    }
    while (to < screen.columns() - 1 && isWordCharacter(screen.cellAt(point.row, to + 1).character)) {
        ++to;
    }
    start = from;
    end = to;
}

std::string TerminalPanel::tokenAt(Point point) const {
    term::Screen& screen = current()->screen();
    auto isTokenCharacter = [](uint32_t c) {
        if (c == 0 || c == ' ') {
            return false;
        }
        if (c > 0x7F) {
            return true;
        }
        return std::strchr("\"'`<>|{}[]^", static_cast<int>(c)) == nullptr && c > ' ';
    };
    if (!isTokenCharacter(screen.cellAt(point.row, point.column).character)) {
        return std::string();
    }
    int from = point.column, to = point.column;
    while (from > 0 && isTokenCharacter(screen.cellAt(point.row, from - 1).character)) {
        --from;
    }
    while (to < screen.columns() - 1 && isTokenCharacter(screen.cellAt(point.row, to + 1).character)) {
        ++to;
    }
    std::string token = screen.textInRange(point.row, from, point.row, to + 1);
    while (!token.empty() && std::strchr(".,;!?", token.back()) != nullptr) {
        token.pop_back();
    }
    while (!token.empty() && token.front() == '(') {
        token.erase(token.begin());
    }
    while (!token.empty() && token.back() == ')' && token.find('(') == std::string::npos) {
        token.pop_back();
    }
    return token;
}

void TerminalPanel::openLinkAt(Point point) {
    std::string token = tokenAt(point);
    if (token.empty()) {
        return;
    }
    if (token.rfind("http://", 0) == 0 || token.rfind("https://", 0) == 0) {
        openExternally(token);
        return;
    }
    // path, path:line, path:line:column, or path(line,column) (MSVC's form).
    static const std::regex withColons(R"(^(.+?):(\d+)(?::(\d+))?:?$)");
    static const std::regex withParentheses(R"(^(.+?)\((\d+)(?:,(\d+))?\):?$)");
    std::smatch match;
    std::string path = token;
    int line = 0, column = 0;
    if (std::regex_match(token, match, withParentheses) || std::regex_match(token, match, withColons)) {
        // A Windows drive letter ("C:") isn't a line number.
        if (!(match[1].str().size() == 1 && std::isalpha(static_cast<unsigned char>(match[1].str()[0])))) {
            path = match[1].str();
            line = std::stoi(match[2].str());
            column = match[3].matched ? std::stoi(match[3].str()) : 0;
        }
    }
    std::filesystem::path candidate = pathFromUtf8(path);
    if (candidate.is_relative()) {
        std::string base = current()->screen().workingDirectory();
        if (base.empty()) {
            base = projectFolder;
        }
        candidate = pathFromUtf8(base) / candidate;
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) && onOpenPath) {
        onOpenPath(utf8FromPath(candidate.lexically_normal()), line, column);
    }
}

bool TerminalPanel::mouseDown(const MouseEvent& event) {
    if (!frame_.contains(event.x, event.y)) {
        return false;
    }
    if (stripRect().contains(event.x, event.y)) {
        for (const StripButton& button : stripButtons_) {
            if (!button.rect.contains(event.x, event.y)) {
                continue;
            }
            if (button.action >= 0) {
                select(static_cast<size_t>(button.action));
            } else if (button.action == -1) {
                newTerminal();
            } else if (button.action == -2) {
                closeCurrentTerminal();
            } else if (button.action == -3 && onHide) {
                onHide();
            }
            return true;
        }
        return true;
    }
    TerminalSession* session = current();
    if (session == nullptr) {
        return true;
    }
    if (event.button == 3) {
        if (!reportMouse(event, 2, true, false) && onContextMenu) {
            onContextMenu(event.x, event.y);
        }
        return true;
    }
    if (event.button != 1) {
        reportMouse(event, 1, true, false);
        return true;
    }
    if (reportMouse(event, 0, true, false)) {
        reportingMouse_ = true;
        return true;
    }
    Point point = pointAt(event.x, event.y);
    if (event.control) {
        openLinkAt(point);
        return true;
    }
    selecting_ = true;
    granularity_ = std::min(event.clicks, 3);
    if (granularity_ == 2) {
        int start = 0, end = 0;
        wordRangeAt(point, start, end);
        anchor_ = {point.row, start};
        head_ = {point.row, end};
    } else if (granularity_ >= 3) {
        anchor_ = {point.row, 0};
        head_ = {point.row, session->screen().columns() - 1};
    } else {
        anchor_ = head_ = point;
    }
    hasSelection_ = granularity_ > 1;
    return true;
}

void TerminalPanel::mouseDragged(const MouseEvent& event) {
    TerminalSession* session = current();
    if (session == nullptr) {
        return;
    }
    if (reportingMouse_) {
        auto mode = session->screen().mouseMode();
        if (mode == term::MouseMode::Drag || mode == term::MouseMode::Motion) {
            reportMouse(event, 0, true, true);
        }
        return;
    }
    if (!selecting_) {
        return;
    }
    ui::Rect area = screenRect();
    if (event.y < area.y) {
        scrollOffset_ = std::min<long>(scrollOffset_ + 1, static_cast<long>(session->screen().historyCount()));
    } else if (event.y > area.bottom()) {
        scrollOffset_ = std::max<long>(scrollOffset_ - 1, 0);
    }
    Point point = pointAt(event.x, event.y);
    auto before = [](Point a, Point b) { return a.row < b.row || (a.row == b.row && a.column < b.column); };
    if (granularity_ == 2) {
        int start = 0, end = 0;
        wordRangeAt(point, start, end);
        head_ = before(point, anchor_) ? Point{point.row, start} : Point{point.row, end};
    } else if (granularity_ >= 3) {
        head_ = {point.row, before(point, anchor_) ? 0 : session->screen().columns() - 1};
    } else {
        head_ = point;
    }
    hasSelection_ = true;
}

void TerminalPanel::mouseUp(const MouseEvent& event) {
    if (reportingMouse_) {
        reportMouse(event, 0, false, false);
        reportingMouse_ = false;
        return;
    }
    if (selecting_ && granularity_ == 1 && anchor_.row == head_.row && anchor_.column == head_.column) {
        hasSelection_ = false;  // a plain click selects nothing
    }
    selecting_ = false;
}

bool TerminalPanel::wheel(const WheelEvent& event) {
    TerminalSession* session = current();
    if (session == nullptr || !frame_.contains(event.x, event.y)) {
        return false;
    }
    term::Screen& screen = session->screen();
    scrollRemainder_ += event.dy * 3;
    long whole = static_cast<long>(scrollRemainder_);
    if (whole == 0) {
        return true;
    }
    scrollRemainder_ -= static_cast<double>(whole);
    MouseEvent at;
    at.x = event.x;
    at.y = event.y;
    at.control = event.control;
    if (screen.mouseMode() != term::MouseMode::None && !event.shift) {
        for (long step = 0; step < std::labs(whole); ++step) {
            reportMouse(at, whole > 0 ? 64 : 65, true, false);
        }
        return true;
    }
    if (screen.onAlternateScreen()) {
        // Full-screen programs without mouse support (less, man) scroll with the arrow keys.
        std::string key = std::string(screen.applicationCursorKeys() ? "\x1bO" : "\x1b[") + (whole > 0 ? "A" : "B");
        std::string keys;
        for (long step = 0; step < std::min<long>(std::labs(whole), 10); ++step) {
            keys += key;
        }
        session->send(keys);
        return true;
    }
    scrollOffset_ = std::clamp<long>(scrollOffset_ + whole, 0, static_cast<long>(screen.historyCount()));
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
    return true;
}

}  // namespace gw::app
