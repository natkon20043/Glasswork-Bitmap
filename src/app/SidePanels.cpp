#include "SidePanels.hpp"

#include "Font.hpp"
#include "Services.hpp"

#include <algorithm>
#include <filesystem>

namespace gw::app {

namespace {

int Pixel() { return Services::shared().chromePixel(); }
int Cell() { return 6 * Pixel(); }
int Line() { return 12 * Pixel(); }
ui::Font& Chrome() { return Services::shared().chromeFont(); }
const bitmap::Scheme& Colors() { return Services::shared().scheme(); }

/// Text cut to fit `cells`, with an ellipsis.
std::string fitted(const std::string& text, int cells) {
    if (ui::textCells(text) <= cells || cells < 2) {
        return text;
    }
    std::string result;
    int used = 0;
    for (size_t index = 0; index < text.size();) {
        size_t start = index;
        char32_t character = ui::decodeUtf8(text, index);
        int width = ui::Font::cells(character);
        if (used + width > cells - 1) {
            break;
        }
        result += text.substr(start, index - start);
        used += width;
    }
    return result + "…";
}

}  // namespace

// ---------------------------------------------------------------------------
// Lists

void ListView::setRows(std::vector<ListRow> rows) {
    rows_ = std::move(rows);
    if (scroll_ >= rows_.size()) {
        scroll_ = 0;
    }
    if (cursor_ >= static_cast<long>(rows_.size())) {
        cursor_ = -1;
    }
}

int ListView::visibleRows() const {
    return std::max(1, frame_.h / Line());
}

int ListView::rowAt(int x, int y) const {
    if (!frame_.contains(x, y)) {
        return -1;
    }
    size_t index = scroll_ + static_cast<size_t>((y - frame_.y) / Line());
    return index < rows_.size() ? static_cast<int>(index) : -1;
}

void ListView::ensureVisible(size_t index) {
    size_t visible = static_cast<size_t>(visibleRows());
    if (index < scroll_) {
        scroll_ = index;
    } else if (index >= scroll_ + visible) {
        scroll_ = index - visible + 1;
    }
}

void ListView::draw(ui::Canvas& canvas, bool focused) {
    const bitmap::Scheme& colors = Colors();
    int pixel = Pixel(), line = Line();
    canvas.pushClip(frame_);
    int iconColumn = 10 * pixel;
    int cells = (frame_.w - iconColumn - 3 * pixel) / Cell();
    for (size_t index = scroll_; index < rows_.size(); ++index) {
        int top = frame_.y + static_cast<int>(index - scroll_) * line;
        if (top >= frame_.bottom()) {
            break;
        }
        const ListRow& row = rows_[index];
        bool highlighted = row.highlighted || (focused && static_cast<long>(index) == cursor_);
        if (highlighted) {
            canvas.fill({frame_.x, top, frame_.w, line}, colors.selection);
        }
        int x = frame_.x + pixel;
        if (row.hasIcon) {
            ui::Rect size = ui::iconSize(row.icon, pixel);
            ui::drawIcon(canvas, row.icon, x + (iconColumn - size.w) / 2 / pixel * pixel, top + (line - size.h) / 2 / pixel * pixel, pixel,
                         row.iconColor, true);
            x += iconColumn + 2 * pixel;
        }
        std::string text = fitted(row.text, row.hasIcon ? cells : (frame_.w - pixel) / Cell());
        if (highlighted) {
            ui::drawText(canvas, Chrome(), x, top, text, colors.selectionText, false);
        } else {
            ui::drawText(canvas, Chrome(), x, top, text, row.textColor, true);
        }
    }
    canvas.popClip();
}

bool ListView::mouseDown(const MouseEvent& event) {
    if (!frame_.contains(event.x, event.y)) {
        return false;
    }
    int index = rowAt(event.x, event.y);
    if (event.button == 3) {
        if (onContextMenu) {
            onContextMenu(index, event.x, event.y);
        }
        return true;
    }
    if (index >= 0) {
        cursor_ = index;
        if (onChoose) {
            ListRow row = rows_[static_cast<size_t>(index)];
            onChoose(row);
        }
    }
    return true;
}

bool ListView::wheel(const WheelEvent& event) {
    if (!frame_.contains(event.x, event.y)) {
        return false;
    }
    long visible = visibleRows();
    long maxScroll = std::max(0L, static_cast<long>(rows_.size()) - visible);
    long next = static_cast<long>(scroll_) - static_cast<long>(std::lround(event.dy * 3));
    scroll_ = static_cast<size_t>(std::clamp(next, 0L, maxScroll));
    return true;
}

bool ListView::keyDown(const KeyPress& key) {
    if (rows_.empty()) {
        return false;
    }
    if (key.key == "up" || key.key == "down" || key.key == "pageup" || key.key == "pagedown" || key.key == "home" || key.key == "end") {
        long step = key.key == "up" ? -1 : key.key == "down" ? 1 : key.key == "pageup" ? -visibleRows() : visibleRows();
        if (key.key == "home") {
            cursor_ = 0;
        } else if (key.key == "end") {
            cursor_ = static_cast<long>(rows_.size()) - 1;
        } else {
            cursor_ = std::clamp(cursor_ + step, 0L, static_cast<long>(rows_.size()) - 1);
        }
        ensureVisible(static_cast<size_t>(cursor_));
        return true;
    }
    if (key.key == "enter" && cursor_ >= 0 && onChoose) {
        ListRow row = rows_[static_cast<size_t>(cursor_)];
        onChoose(row);
        return true;
    }
    if (key.key == "backspace" && onChoose) {
        for (const ListRow& row : rows_) {
            if (row.payload == "..") {
                ListRow up = row;
                onChoose(up);
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The debugger

DebugPanel::DebugPanel(Debugger& debugger) : debugger_(debugger) {}

void DebugPanel::changed() {
    if (onNeedsDisplay) {
        onNeedsDisplay();
    }
}

void DebugPanel::reload() {
    if (debugger_.state() == DebugState::Stopped) {
        debugger_.fetchScopes([this](std::vector<std::shared_ptr<DebugVariable>> scopes) {
            scopes_ = std::move(scopes);
            // The first scope (the locals) opens by itself.
            if (!scopes_.empty()) {
                auto first = scopes_.front();
                first->expanded = true;
                debugger_.fetchChildren(first, [this] { changed(); });
            }
            changed();
        });
    } else {
        scopes_.clear();
    }
    stackScroll_ = 0;
    changed();
}

void DebugPanel::appendConsole(const std::string& text, const std::string& category) {
    const bitmap::Scheme& colors = Colors();
    Color color = category == "stderr" ? colors.error : category == "console" ? colors.textDim : colors.text;
    std::string combined = (partialCategory_ == category ? partialLine_ : std::string()) + text;
    if (partialCategory_ != category && !partialLine_.empty()) {
        console_.push_back({partialLine_, color});
    }
    partialLine_.clear();
    size_t start = 0;
    while (true) {
        size_t end = combined.find('\n', start);
        if (end == std::string::npos) {
            partialLine_ = combined.substr(start);
            partialCategory_ = category;
            break;
        }
        std::string line = combined.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        console_.push_back({line, color});
        start = end + 1;
    }
    if (console_.size() > 5000) {
        console_.erase(console_.begin(), console_.begin() + static_cast<long>(console_.size() - 5000));
    }
    consoleScroll_ = 0;
    changed();
}

void DebugPanel::clearConsole() {
    console_.clear();
    partialLine_.clear();
    changed();
}

std::vector<DebugPanel::VariableRow> DebugPanel::visibleVariables() const {
    std::vector<VariableRow> rows;
    std::function<void(const std::shared_ptr<DebugVariable>&, int)> add = [&](const std::shared_ptr<DebugVariable>& variable, int depth) {
        rows.push_back({variable, depth});
        if (variable->expanded) {
            for (const auto& child : variable->children) {
                add(child, depth + 1);
            }
        }
    };
    for (const auto& scope : scopes_) {
        add(scope, 0);
    }
    return rows;
}

void DebugPanel::draw(ui::Canvas& canvas, bool focused) {
    const bitmap::Scheme& colors = Colors();
    int pixel = Pixel(), cell = Cell(), line = Line();
    canvas.pushClip(frame_);
    hits_.clear();
    int cells = std::max(4, frame_.w / cell);
    int y = frame_.y;
    ui::drawText(canvas, Chrome(), frame_.x, y, "RUN AND DEBUG", colors.textAccent, true);
    y += line;
    DebugState state = debugger_.state();
    const char* status = state == DebugState::Idle ? "Not running" : state == DebugState::Starting ? "Starting…"
                         : state == DebugState::Running                                     ? "Running"
                                                                                            : "Paused";
    std::string statusText = status;
    if (state != DebugState::Idle && !debugger_.adapterName().empty()) {
        statusText += " (" + debugger_.adapterName() + ")";
    }
    ui::drawText(canvas, Chrome(), frame_.x, y, fitted(statusText, cells), colors.text, true);
    y += line;
    if (state == DebugState::Idle) {
        y += line / 2;
        std::string start = "→ Start Debugging";
        int width = ui::textWidth(Chrome(), start);
        ui::drawText(canvas, Chrome(), frame_.x, y, start, colors.text, true);
        hits_.push_back({{frame_.x, y, width, line}, 5, 0});
        y += line + line / 2;
        for (const std::string& piece : ui::wrapText("Click left of the line numbers (or press F9) to set breakpoints. "
                                                      "F5 builds and runs the current file (or the program you chose) "
                                                      "under the debugger.",
                                                      static_cast<size_t>(cells))) {
            ui::drawText(canvas, Chrome(), frame_.x, y, piece, colors.textDim, true);
            y += line;
        }
        y += line / 2;
    } else {
        // The buttons: continue, pause, step over, into, out, stop.
        const char* labels[] = {"▶", "||", "→", "↓", "↑", "■"};
        int x = frame_.x;
        for (int index = 0; index < 6; ++index) {
            std::string label = std::string("[") + labels[index] + "]";
            int width = ui::textWidth(Chrome(), label);
            bool available = (index == 0 || index >= 2) ? state == DebugState::Stopped || index == 5 : state == DebugState::Running;
            ui::drawText(canvas, Chrome(), x, y, label, available ? colors.text : colors.textDim, true);
            hits_.push_back({{x, y, width, line}, 1, index});
            x += width;
        }
        y += line + line / 2;
    }
    // The sections share what is left: the stack a quarter, the variables a third, the console the rest.
    int remaining = frame_.bottom() - y;
    int stackHeight = std::max(2 * line, remaining / 4 / line * line);
    int variableHeight = std::max(2 * line, remaining / 3 / line * line);
    ui::drawText(canvas, Chrome(), frame_.x, y, "CALL STACK", colors.textAccent, true);
    y += line;
    stackArea_ = {frame_.x, y, frame_.w, stackHeight - line};
    const auto& frames = debugger_.frames();
    for (size_t index = stackScroll_; index < frames.size(); ++index) {
        int top = stackArea_.y + static_cast<int>(index - stackScroll_) * line;
        if (top + line > stackArea_.bottom()) {
            break;
        }
        const DebugFrame& frame = frames[index];
        std::string text = frame.name;
        if (!frame.path.empty()) {
            text += "  " + std::filesystem::path(frame.path).filename().string() + ":" + std::to_string(frame.line);
        }
        bool selected = index == debugger_.selectedFrame();
        if (selected) {
            canvas.fill({frame_.x, top, frame_.w, line}, colors.selection);
        }
        ui::drawText(canvas, Chrome(), frame_.x + cell / 2, top, fitted(text, cells - 1), selected ? colors.selectionText : colors.text,
                     !selected);
        hits_.push_back({{frame_.x, top, frame_.w, line}, 2, static_cast<int>(index)});
    }
    y = stackArea_.bottom();
    ui::drawText(canvas, Chrome(), frame_.x, y, "VARIABLES", colors.textAccent, true);
    y += line;
    variableArea_ = {frame_.x, y, frame_.w, variableHeight - line};
    std::vector<VariableRow> variables = visibleVariables();
    for (size_t index = variableScroll_; index < variables.size(); ++index) {
        int top = variableArea_.y + static_cast<int>(index - variableScroll_) * line;
        if (top + line > variableArea_.bottom()) {
            break;
        }
        const VariableRow& row = variables[index];
        int indent = row.depth * cell;
        std::string marker = row.variable->reference != 0 ? (row.variable->expanded ? "v " : "> ") : "  ";
        std::string text = marker + row.variable->name;
        if (!row.variable->value.empty()) {
            text += " = " + row.variable->value;
        }
        ui::drawText(canvas, Chrome(), frame_.x + indent, top, fitted(text, cells - row.depth),
                     row.depth == 0 ? colors.link : colors.text, true);
        hits_.push_back({{frame_.x, top, frame_.w, line}, 3, static_cast<int>(index)});
    }
    y = variableArea_.bottom();
    ui::drawText(canvas, Chrome(), frame_.x, y, "CONSOLE", colors.textAccent, true);
    y += line;
    ui::Rect inputRect{frame_.x, frame_.bottom() - line, frame_.w, line};
    consoleArea_ = {frame_.x, y, frame_.w, std::max(0, inputRect.y - y - pixel)};
    // The console's lines, wrapped, newest at the bottom.
    std::vector<std::pair<std::string, Color>> wrapped;
    for (const ConsoleLine& entry : console_) {
        for (const std::string& piece : ui::wrapText(entry.text, static_cast<size_t>(cells))) {
            wrapped.emplace_back(piece, entry.color);
        }
    }
    if (!partialLine_.empty()) {
        wrapped.emplace_back(partialLine_, colors.text);
    }
    int capacity = std::max(0, consoleArea_.h / line);
    size_t end = wrapped.size() > consoleScroll_ ? wrapped.size() - consoleScroll_ : 0;
    size_t begin = end > static_cast<size_t>(capacity) ? end - static_cast<size_t>(capacity) : 0;
    int top = consoleArea_.y;
    for (size_t index = begin; index < end; ++index) {
        ui::drawText(canvas, Chrome(), frame_.x, top, wrapped[index].first, wrapped[index].second, true);
        top += line;
    }
    canvas.fill(inputRect, colors.input);
    input_.draw(canvas, Chrome(), inputRect.x + pixel, inputRect.y, inputRect.w - 2 * pixel, colors.inputText, colors.barHighlight,
                colors.barHighlightText, colors.inputText, focused && inputFocused_, false, "Evaluate…", colors.barDim);
    hits_.push_back({inputRect, 4, 0});
    canvas.popClip();
}

bool DebugPanel::mouseDown(const MouseEvent& event) {
    if (!frame_.contains(event.x, event.y)) {
        return false;
    }
    if (onFocus) {
        onFocus();
    }
    for (const Hit& hit : hits_) {
        if (!hit.rect.contains(event.x, event.y)) {
            continue;
        }
        inputFocused_ = hit.kind == 4;
        switch (hit.kind) {
            case 1:
                switch (hit.index) {
                    case 0: debugger_.resume(); break;
                    case 1: debugger_.pause(); break;
                    case 2: debugger_.stepOver(); break;
                    case 3: debugger_.stepInto(); break;
                    case 4: debugger_.stepOut(); break;
                    case 5: debugger_.stop(); break;
                    default: break;
                }
                break;
            case 2:
                debugger_.selectFrame(static_cast<size_t>(hit.index));
                reload();
                break;
            case 3: {
                std::vector<VariableRow> rows = visibleVariables();
                if (static_cast<size_t>(hit.index) < rows.size()) {
                    auto variable = rows[static_cast<size_t>(hit.index)].variable;
                    if (variable->reference != 0) {
                        variable->expanded = !variable->expanded;
                        if (variable->expanded && !variable->fetched) {
                            debugger_.fetchChildren(variable, [this] { changed(); });
                        }
                    }
                }
                break;
            }
            case 4:
                input_.clickAt(event.x - hit.rect.x - Pixel(), Chrome().cellWidth(), event.shift);
                break;
            case 5:
                if (onStart) {
                    onStart();
                }
                break;
            default:
                break;
        }
        changed();
        return true;
    }
    inputFocused_ = false;
    changed();
    return true;
}

bool DebugPanel::wheel(const WheelEvent& event) {
    if (!frame_.contains(event.x, event.y)) {
        return false;
    }
    long step = -static_cast<long>(std::lround(event.dy * 3));
    if (stackArea_.contains(event.x, event.y)) {
        long maxScroll = std::max(0L, static_cast<long>(debugger_.frames().size()) - stackArea_.h / Line());
        stackScroll_ = static_cast<size_t>(std::clamp(static_cast<long>(stackScroll_) + step, 0L, maxScroll));
    } else if (variableArea_.contains(event.x, event.y)) {
        long maxScroll = std::max(0L, static_cast<long>(visibleVariables().size()) - variableArea_.h / Line());
        variableScroll_ = static_cast<size_t>(std::clamp(static_cast<long>(variableScroll_) + step, 0L, maxScroll));
    } else if (consoleArea_.contains(event.x, event.y)) {
        consoleScroll_ = static_cast<size_t>(std::clamp(static_cast<long>(consoleScroll_) - step, 0L, static_cast<long>(console_.size())));
    }
    changed();
    return true;
}

bool DebugPanel::keyDown(const KeyPress& key) {
    if (!inputFocused_) {
        return false;
    }
    if (key.key == "escape") {
        inputFocused_ = false;
        changed();
        return true;
    }
    if (key.key == "enter") {
        std::string expression = input_.text();
        if (expression.empty()) {
            return true;
        }
        input_.setText("");
        appendConsole("> " + expression + "\n", "console");
        debugger_.evaluate(expression, [this](std::string result, std::string error) {
            if (error.empty()) {
                appendConsole(result + "\n", "stdout");
            } else {
                appendConsole(error + "\n", "stderr");
            }
        });
        return true;
    }
    bool used = input_.keyDown(key);
    changed();
    return used;
}

void DebugPanel::textInput(const std::string& text) {
    input_.insert(text);
    changed();
}

}  // namespace gw::app
