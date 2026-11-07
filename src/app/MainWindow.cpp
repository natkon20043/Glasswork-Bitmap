#include "MainWindow.hpp"

#include "App.hpp"
#include "BitmapMode.hpp"
#include "Font.hpp"
#include "Git.hpp"
#include "Loop.hpp"
#include "Markdown.hpp"
#include "Paths.hpp"
#include "PixelArt.hpp"
#include "Platform.hpp"
#include "Services.hpp"
#include "Image.hpp"

#include <fstream>

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
int Snap(double value) { return static_cast<int>(std::floor(value / Pixel())) * Pixel(); }
int PanelWidth() { return 25 * Cell(); }

const char* const kPanelNames[] = {"Files", "Issues", "Debug"};

/// The status line's keys: label and command.
struct Hint {
    const char* label;
    const char* command;
    const char* fallbackKey;
};
const Hint kHints[] = {
    {"Save", "workbench.action.files.save", "^S"},
    {"Run", "workbench.action.debug.start", "F5"},
    {"Break", "editor.debug.action.toggleBreakpoint", "F9"},
    {"Open", "workbench.action.quickOpen", "^P"},
    {"Find", "actions.find", "^F"},
    {"Term", "workbench.action.terminal.toggleTerminal", "^`"},
};

/// SDL's key to VS Code's name for it.
std::string keyName(SDL_Keycode key, bool* keypad) {
    *keypad = false;
    switch (key) {
        case SDLK_RETURN: return "enter";
        case SDLK_KP_ENTER: *keypad = true; return "enter";
        case SDLK_ESCAPE: return "escape";
        case SDLK_BACKSPACE: return "backspace";
        case SDLK_TAB: return "tab";
        case SDLK_SPACE: return "space";
        case SDLK_DELETE: return "delete";
        case SDLK_INSERT: return "insert";
        case SDLK_HOME: return "home";
        case SDLK_END: return "end";
        case SDLK_PAGEUP: return "pageup";
        case SDLK_PAGEDOWN: return "pagedown";
        case SDLK_UP: return "up";
        case SDLK_DOWN: return "down";
        case SDLK_LEFT: return "left";
        case SDLK_RIGHT: return "right";
        case SDLK_KP_PLUS: *keypad = true; return "numpad_add";
        case SDLK_KP_MINUS: *keypad = true; return "numpad_subtract";
        case SDLK_KP_MULTIPLY: *keypad = true; return "numpad_multiply";
        case SDLK_KP_DIVIDE: *keypad = true; return "numpad_divide";
        default: break;
    }
    if (key >= SDLK_F1 && key <= SDLK_F12) {
        return "f" + std::to_string(key - SDLK_F1 + 1);
    }
    if (key >= SDLK_F13 && key <= SDLK_F24) {
        return "f" + std::to_string(key - SDLK_F13 + 13);
    }
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
        *keypad = true;
        return std::string(1, static_cast<char>('1' + (key - SDLK_KP_1)));
    }
    if (key == SDLK_KP_0) {
        *keypad = true;
        return "0";
    }
    if (key > 32 && key < 127) {
        char character = static_cast<char>(key);
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        return std::string(1, character);
    }
    return std::string();
}

}  // namespace

std::string Tab::path() const {
    if (kind == Kind::Preview) {
        return std::string();
    }
    if (kind == Kind::Image) {
        return previewOf;
    }
    if (document && document->path()) {
        return utf8FromPath(*document->path());
    }
    return std::string();
}

std::string Tab::title() const {
    if (kind == Kind::Preview) {
        return "Preview " + utf8FromPath(pathFromUtf8(previewOf).filename());
    }
    return document ? document->displayName() : std::string("Untitled");
}

// ---------------------------------------------------------------------------
// Making the window

MainWindow::MainWindow(App& app, const std::string& folder, const std::vector<std::string>& files) : app_(app) {
    createWindow();
    findBar_ = std::make_unique<FindBar>(editor_);
    debugPanel_ = std::make_unique<DebugPanel>(debugger_);
    terminalHeight_ = 15 * Line();
    std::weak_ptr<bool> alive = alive_;

    editor_.onNeedsDisplay = [this] { dirty_ = true; };
    editor_.onSelectionChange = [this] {
        if (suggestions_.visible() && !caretInWordStartingAt(suggestionWordStart_)) {
            suggestions_.hide();
            ++suggestionToken_;
        }
        if (signatureLine_ >= 0 && currentTab() && static_cast<long>(currentTab()->document->selection.caret.line) != signatureLine_) {
            hideHint();
        }
        updateStatus();
        scheduleEditorStateForExtensions();
        dirty_ = true;
    };
    editor_.onEdit = [this] {
        if (Tab* tab = currentTab()) {
            documentEdited(*tab);
        }
    };
    editor_.onTypedText = [this](const std::optional<std::string>& typed) { editorTypedText(typed); };
    editor_.onHoverRequest = [this](Position position, Range word) { hoverAt(position, word); };
    editor_.onHoverEnd = [this] {
        ++hoverToken_;
        if (signatureLine_ < 0) {
            hover_.hide();
        }
        dirty_ = true;
    };
    editor_.onCompletionKey = [this](const std::string& action) {
        if (!suggestions_.visible()) {
            return false;
        }
        if (!caretInWordStartingAt(suggestionWordStart_)) {
            suggestions_.hide();
            ++suggestionToken_;
            return false;
        }
        bool used = suggestions_.handleKey(action);
        dirty_ = true;
        return used;
    };
    editor_.onBreakpointsChange = [this] { breakpointsChanged(); };
    editor_.onChangeMarkerClick = [this](size_t line, ui::Rect bar) { changeBarClicked(line, bar); };
    editor_.onVimStatusChange = [this] {
        if (!editor_.insertsText()) {
            suggestions_.hide();
            ++suggestionToken_;
        }
        updateStatus();
    };
    editor_.onVimCommand = [this](const std::string& command) { return runVimCommand(command); };
    editor_.onVimAction = [this](VimAction action) { vimAction(action); };
    editor_.onSectionsChange = [this] { dirty_ = true; };
    editor_.onContextMenu = [this](int x, int y) {
        Menu menu;
        auto item = [](const char* title, const char* command) {
            MenuItem entry;
            entry.title = title;
            entry.command = command;
            return entry;
        };
        menu.items = {
            item("Cu&t", "editor.action.clipboardCutAction"),
            item("&Copy", "editor.action.clipboardCopyAction"),
            item("&Paste", "editor.action.clipboardPasteAction"),
            MenuItem::separator(),
            item("Go to &Definition", "editor.action.revealDefinition"),
            item("Find &References", "editor.action.goToReferences"),
            item("Re&name Symbol", "editor.action.rename"),
            item("&Quick Fix", "editor.action.quickFix"),
            item("&Format Document", "editor.action.formatDocument"),
            MenuItem::separator(),
            item("Toggle &Breakpoint", "editor.debug.action.toggleBreakpoint"),
            item("Toggle Co&mment", "editor.action.commentLine"),
        };
        pushOverlay(std::make_unique<MenuOverlay>(menu, x, y, menuHost()));
    };

    findBar_->onNeedsDisplay = [this] { dirty_ = true; };
    findBar_->onFocus = [this] { setFocus(Focus::Find); };
    findBar_->onClose = [this] {
        if (focus_ == Focus::Find) {
            setFocus(Focus::Editor);
        }
        layout();
    };

    terminal_.onNeedsDisplay = [this] { dirty_ = true; };
    terminal_.onOpenPath = [this](const std::string& path, int line, int column) {
        rememberLocation();
        openPath(path, line, column);
    };
    terminal_.onHide = [this] { setTerminalVisible(false); };
    terminal_.onFocus = [this] { setFocus(Focus::Terminal); };
    terminal_.onContextMenu = [this](int x, int y) {
        Menu menu;
        MenuItem copy;
        copy.title = "&Copy";
        copy.run = [this] { terminal_.copySelection(); };
        copy.enabled = [this] { return terminal_.hasSelection(); };
        MenuItem paste;
        paste.title = "&Paste";
        paste.run = [this] { terminal_.paste(); };
        MenuItem all;
        all.title = "Select &All";
        all.run = [this] { terminal_.selectAll(); };
        MenuItem clear;
        clear.title = "C&lear";
        clear.run = [this] { terminal_.clear(); };
        MenuItem fresh;
        fresh.title = "&New Terminal";
        fresh.command = "workbench.action.terminal.new";
        MenuItem kill;
        kill.title = "&Kill Terminal";
        kill.run = [this] { terminal_.closeCurrentTerminal(); };
        menu.items = {copy, paste, all, MenuItem::separator(), clear, fresh, kill};
        pushOverlay(std::make_unique<MenuOverlay>(menu, x, y, menuHost()));
    };

    list_.onChoose = [this](const ListRow& row) { chooseRow(row); };
    list_.onContextMenu = [this](int row, int x, int y) { fileContextMenu(row, x, y); };
    debugPanel_->onNeedsDisplay = [this] { dirty_ = true; };
    debugPanel_->onFocus = [this] { setFocus(Focus::Panel); };
    debugPanel_->onStart = [this] { startDebugging(); };

    suggestions_.onAccept = [this](std::shared_ptr<Suggestion> suggestion) { acceptSuggestion(suggestion, ""); };
    suggestions_.onSelectionChanged = [this](std::shared_ptr<Suggestion> suggestion) {
        Tab* tab = currentTab();
        if (!tab || !suggestion || suggestion->resolved || !suggestion->raw) {
            return;
        }
        uint64_t token = ++resolveToken_;
        Document* document = tab->document.get();
        std::weak_ptr<bool> weak = alive_;
        afterDelay(0.12, [this, weak, token, suggestion, document] {
            if (weak.expired() || token != resolveToken_ || suggestions_.selected() != suggestion) {
                return;
            }
            resolveSuggestion(suggestion, document, [this] { dirty_ = true; });
        });
    };
    changeBox_.onRevert = [this] {
        Tab* tab = currentTab();
        if (!tab || !tab->headText) {
            return;
        }
        for (const DiffHunk& hunk : tab->hunks) {
            bool here = changeLine_ >= hunk.newStart && changeLine_ < hunk.newStart + std::max<size_t>(1, hunk.newCount);
            if (!here) {
                continue;
            }
            std::vector<std::string_view> old = splitLines(*tab->headText);
            std::string replacement;
            for (size_t index = hunk.oldStart; index < hunk.oldStart + hunk.oldCount && index < old.size(); ++index) {
                replacement += std::string(old[index]) + "\n";
            }
            const TextBuffer& buffer = tab->document->buffer();
            Range range;
            if (hunk.newCount == 0) {
                range = {{hunk.newStart, 0}, {hunk.newStart, 0}};
                if (hunk.newStart >= buffer.lineCount()) {
                    size_t last = buffer.lineCount() - 1;
                    range = {{last, buffer.line(last).size()}, {last, buffer.line(last).size()}};
                    if (!replacement.empty()) {
                        replacement = "\n" + replacement.substr(0, replacement.size() - 1);
                    }
                }
            } else {
                size_t end = hunk.newStart + hunk.newCount;
                if (end < buffer.lineCount()) {
                    range = {{hunk.newStart, 0}, {end, 0}};
                } else {
                    size_t last = buffer.lineCount() - 1;
                    range = {{hunk.newStart, 0}, {last, buffer.line(last).size()}};
                    if (!replacement.empty() && replacement.back() == '\n') {
                        replacement.pop_back();
                    }
                    if (hunk.newStart > 0 && replacement.empty()) {
                        range.start = {hunk.newStart - 1, buffer.line(hunk.newStart - 1).size()};
                    }
                }
            }
            editor_.replaceRange(range, replacement);
            break;
        }
        changeBox_.hide();
        dirty_ = true;
    };
    changeBox_.onStage = [this] {
        Tab* tab = currentTab();
        if (!tab || !tab->headText || gitRoot_.empty() || tab->document->isDirty()) {
            if (tab && tab->document->isDirty()) {
                setStatusMessage("Save the file before staging a change");
            }
            changeBox_.hide();
            return;
        }
        std::string relative = genericUtf8(pathFromUtf8(tab->path()).lexically_relative(pathFromUtf8(gitRoot_)));
        std::vector<std::string_view> old = splitLines(*tab->headText);
        std::string text = tab->document->buffer().text();
        std::vector<std::string_view> now = splitLines(text);
        for (const DiffHunk& hunk : tab->hunks) {
            bool here = changeLine_ >= hunk.newStart && changeLine_ < hunk.newStart + std::max<size_t>(1, hunk.newCount);
            if (here) {
                git::CommandResult result = git::applyPatch(gitRoot_, patchForHunk(relative, old, now, hunk), true, false);
                setStatusMessage(result.ok ? "Change staged" : "Couldn't stage: " + result.error);
                break;
            }
        }
        changeBox_.hide();
        refreshGit(*tab);
    };
    changeBox_.onNext = [this] {
        changeBox_.hide();
        jumpToChange(1);
    };
    changeBox_.onPrevious = [this] {
        changeBox_.hide();
        jumpToChange(-1);
    };

    setUpLanguage();
    setUpDebugger();
    registerCommands();
    listener_ = Services::shared().addListener([this, alive](ServicesChange change) {
        if (alive.expired()) {
            return;
        }
        if (change == ServicesChange::Extensions) {
            for (auto& tab : tabs_) {
                if (tab->kind == Tab::Kind::Text && tab->document) {
                    if (!tab->path().empty()) {
                        if (auto language = Services::shared().extensions().languageForFile(pathFromUtf8(tab->path()))) {
                            tab->document->languageId = *language;
                        }
                    }
                    tab->highlighter->setGrammar(Services::shared().grammars().forLanguage(tab->document->languageId));
                    tab->configuration = Services::shared().extensions().configuration(tab->document->languageId);
                }
            }
            if (extensionHost_) {
                extensionHost_->extensionsDidChange();
            }
            showTab(selected_);
        }
        if (change == ServicesChange::Settings || change == ServicesChange::Colors || change == ServicesChange::Fonts) {
            language_->enabled = Services::shared().settings().languageServers;
            language_->settingsDidChange(extensionHost_->allSettings());
            extensionHost_->settingsDidChange();
            editor_.reloadAppearance();
            terminal_.reloadAppearance();
            if (Tab* tab = currentTab(); tab && tab->kind == Tab::Kind::Text) {
                applyEditorConfig(*tab);
            }
        }
        layout();
        reloadSidePanel();
        dirty_ = true;
    });

    nlohmann::json panelOpen = Services::shared().value("panelOpen");
    panelOpen_ = panelOpen.is_boolean() ? panelOpen.get<bool>() : true;
    nlohmann::json height = Services::shared().value("terminalLines");
    if (height.is_number_integer()) {
        terminalHeight_ = std::max(6, height.get<int>()) * Line();
    }
    if (!folder.empty()) {
        openFolder(folder);
    }
    for (const std::string& file : files) {
        openPath(file);
    }
    updateTitle();
    reloadSidePanel();
    layout();
    updateStatus();
    editor_.focusChanged(true);
    if (!scripted_) {
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
    }
}

MainWindow::~MainWindow() {
    alive_.reset();
    Services::shared().removeListener(listener_);
    saveSession();
    watcher_.reset();
    debugger_.stop();
    if (extensionHost_) {
        extensionHost_->shutdown();
    }
    if (language_) {
        language_->shutdown();
    }
    for (auto& tab : tabs_) {
        if (tab->document) {
            tab->document->onChange = nullptr;
        }
    }
    editor_.setDocument(nullptr, nullptr, LanguageConfiguration());
    if (texture_) {
        SDL_DestroyTexture(texture_);
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
    }
    if (window_) {
        SDL_DestroyWindow(window_);
    }
}

void MainWindow::createWindow() {
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    float scale = SDL_GetDisplayContentScale(display);
    if (scale <= 0) {
        scale = 1;
    }
    int width = static_cast<int>(1180 * scale), height = static_cast<int>(780 * scale);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (mode) {
        width = std::min(width, mode->w * 9 / 10);
        height = std::min(height, mode->h * 85 / 100);
    }
    // Each new window a little down and to the right of the one before.
    size_t count = app_.windows().size();
    scripted_ = !environmentValue("GLASSWORK_SCRIPT").empty();
    if (scripted_) {
        width = 1200;
        height = 800;
        std::string size = environmentValue("GLASSWORK_SCRIPT_SIZE");
        if (size.find('x') != std::string::npos) {
            width = std::max(320, std::atoi(size.c_str()));
            height = std::max(240, std::atoi(size.c_str() + size.find('x') + 1));
        }
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (!scripted_) {
        flags |= SDL_WINDOW_BORDERLESS;  // the menu bar is the title bar, with pixel window boxes
    }
    window_ = SDL_CreateWindow("Glasswork Bitmap", width, height, flags);
    if (count > 0) {
        int x = 0, y = 0;
        SDL_GetWindowPosition(app_.windows().back()->sdlWindow(), &x, &y);
        SDL_SetWindowPosition(window_, x + 32, y + 32);
    }
    SDL_SetWindowMinimumSize(window_, 480, 320);
    useDarkTitleBar(window_);
    SDL_SetWindowHitTest(window_, &MainWindow::hitTest, this);
    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (renderer_) {
        SDL_SetRenderVSync(renderer_, 1);
    }
    SDL_StartTextInput(window_);
    updateScale();
}

void MainWindow::updateScale() {
    pixelDensity_ = SDL_GetWindowPixelDensity(window_);
    if (pixelDensity_ <= 0) {
        pixelDensity_ = 1;
    }
    float scale = SDL_GetWindowDisplayScale(window_);
    if (scale > 0 && std::abs(scale - Services::shared().displayScale()) > 0.01) {
        Services::shared().setDisplayScale(scale);
    }
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    width = std::max(1, width);
    height = std::max(1, height);
    if (width != canvas_.width() || height != canvas_.height()) {
        canvas_.resize(width, height);
        if (texture_) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        if (renderer_) {
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
            if (texture_) {
                SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
            }
        }
    }
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Geometry

std::vector<ui::Rect> MainWindow::menuTitleRects() const {
    std::vector<ui::Rect> rects;
    int height = std::max(14 * Pixel(), Line());
    int x = Cell();
    static const char* const titles[] = {"≡", "File", "Edit", "Search", "Run", "Debug", "View", "Options", "Help"};
    for (const char* title : titles) {
        int width = std::string(title) == "≡" ? 3 * Cell() : ui::textWidth(Chrome(), title) + 2 * Cell();
        rects.push_back({x, 0, width, height});
        x += width;
    }
    return rects;
}

MainWindow::Geometry MainWindow::geometry() const {
    int pixel = Pixel(), cell = Cell(), line = Line(), shadow = Shadow();
    int width = canvas_.width(), height = canvas_.height();
    Geometry g{};
    g.menuBar = {0, 0, width, std::max(14 * pixel, line)};
    g.status = {0, height - line, width, line};
    int top = g.menuBar.bottom() + 2 * pixel;
    int bottom = g.status.y - shadow - pixel;
    int right = width - shadow - pixel;
    if (panelOpen_) {
        g.panelWindow = {Snap(right - PanelWidth()), top, PanelWidth(), std::max(0, bottom - top)};
        g.panelContent = {g.panelWindow.x + cell, top + line, g.panelWindow.w - 2 * cell, std::max(0, g.panelWindow.h - 2 * line)};
        right = g.panelWindow.x - shadow - 2 * pixel;
    }
    if (terminalVisible_) {
        int terminal = Snap(std::min<double>(terminalHeight_, std::max(0, bottom - top - 8 * line)));
        g.terminalWindow = {2 * pixel, bottom - terminal, std::max(0, right - 2 * pixel), terminal};
        g.terminalContent = {g.terminalWindow.x + cell, g.terminalWindow.y + line, std::max(0, g.terminalWindow.w - 2 * cell),
                             std::max(0, terminal - 2 * line)};
        bottom = g.terminalWindow.y - shadow - 2 * pixel;
    }
    g.editorWindow = {2 * pixel, top, std::max(0, right - 2 * pixel), std::max(0, bottom - top)};
    const ui::Rect& window = g.editorWindow;
    g.editorContent = {window.x + cell, window.y + line, std::max(0, window.w - 2 * cell), std::max(0, window.h - 2 * line)};
    g.closeBox = {window.x + cell, window.y, 3 * cell, line};
    g.zoomBox = {window.right() - 5 * cell, window.y, 3 * cell, line};
    int scrollLeft = window.right() - cell;
    g.upArrow = {scrollLeft, g.editorContent.y, cell, cell};
    g.downArrow = {scrollLeft, g.editorContent.bottom() - cell, cell, cell};
    g.track = {scrollLeft, g.upArrow.bottom(), cell, std::max(0, g.downArrow.y - g.upArrow.bottom())};
    int barLeft = window.x + 2 * cell + std::max(12 * cell, Snap(ui::textWidth(Chrome(), positionLabel())) + cell);
    int barRight = !modeLabel().empty() ? modeLeft(window) - cell : window.right() - 3 * cell;
    if (barRight - barLeft >= 4 * cell) {
        int barTop = window.bottom() - line / 2 - cell / 2;
        g.leftArrow = {barLeft, barTop, cell, cell};
        g.rightArrow = {barRight - cell, barTop, cell, cell};
        g.sideTrack = {g.leftArrow.right(), barTop, g.rightArrow.x - g.leftArrow.right(), cell};
    }
    return g;
}

std::string MainWindow::positionLabel() const {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return std::string();
    }
    Position caret = tab->document->selection.caret;
    std::string_view line = tab->document->buffer().line(caret.line);
    size_t characters = 0;
    for (size_t index = 0; index < std::min(caret.column, line.size()); ++index) {
        if ((static_cast<unsigned char>(line[index]) & 0xC0) != 0x80) {
            ++characters;
        }
    }
    return " " + std::to_string(caret.line + 1) + ":" + std::to_string(characters + 1) + " ";
}

std::string MainWindow::modeLabel() const {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return std::string();
    }
    std::string mode = const_cast<EditorView&>(editor_).vimStatus();
    return mode.empty() ? std::string() : " " + mode + " ";
}

int MainWindow::modeLeft(const ui::Rect& window) const {
    return Snap(window.right() - 3 * Cell() - ui::textWidth(Chrome(), modeLabel()));
}

void MainWindow::layout() {
    Geometry g = geometry();
    editor_.setFrame(g.editorContent);
    findBar_->layout(g.editorContent);
    if (terminalVisible_) {
        terminal_.setFrame(g.terminalContent);
    }
    list_.setFrame(g.panelContent);
    debugPanel_->setFrame(g.panelContent);
    for (auto& overlay : overlays_) {
        overlay->layout(canvas_.bounds());
    }
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Drawing

void MainWindow::drawFrame(const ui::Rect& rect, Color color, bool twoLines) {
    int pixel = Pixel(), line = Line(), cell = Cell();
    auto ring = [&](int inLeft, int inTop, int inRight, int inBottom) {
        int left = rect.x + inLeft, right = rect.right() - inRight - pixel;
        int top = rect.y + inTop, bottom = rect.bottom() - inBottom - pixel;
        canvas_.fillShadowed({left, top, right - left + pixel, pixel}, color, Pixel());
        canvas_.fillShadowed({left, bottom, right - left + pixel, pixel}, color, Pixel());
        canvas_.fillShadowed({left, top, pixel, bottom - top + pixel}, color, Pixel());
        canvas_.fillShadowed({right, top, pixel, bottom - top + pixel}, color, Pixel());
    };
    if (twoLines) {
        ring(2 * pixel, line / 2 - pixel, 2 * pixel, line / 2 - pixel);
        ring(4 * pixel, line / 2 + pixel, 4 * pixel, line / 2 + pixel);
    } else {
        ring(cell / 2, line / 2, cell / 2 - pixel, line / 2 - pixel);
    }
}

int MainWindow::drawOnEdge(const std::string& text, int x, int top, Color color, Color background, bool shadow) {
    int width = ui::textWidth(Chrome(), text);
    canvas_.fill({x, top, width, Line()}, background);
    if (shadow) {
        ui::drawText(canvas_, Chrome(), x, top, text, color, true);
    } else {
        ui::drawTextPlain(canvas_, Chrome(), x, top, text, color);
    }
    return width;
}

void MainWindow::draw() {
    const bitmap::Scheme& colors = Colors();
    Geometry g = geometry();
    canvas_.fillChecker(canvas_.bounds(), colors.desktop, colors.desktopDots, Pixel());
    drawMenuBar(g);
    drawEditorWindow(g);
    Tab* tab = currentTab();
    if (tab && tab->kind == Tab::Kind::Image) {
        drawImageTab(*tab, g.editorContent);
    } else if (tab && tab->kind == Tab::Kind::Preview) {
        drawPreviewTab(*tab, g.editorContent);
    } else {
        editor_.draw(canvas_, focus_ == Focus::Editor && overlays_.empty() && hasKeyboardFocus());
        if (!tab) {
            drawEmptyHint(g.editorContent);
        }
    }
    if (findBar_->visible()) {
        findBar_->draw(canvas_, focus_ == Focus::Find && overlays_.empty());
    }
    if (panelOpen_) {
        drawPanelWindow(g);
        if (panel_ == SidePanel::Debug) {
            debugPanel_->draw(canvas_, focus_ == Focus::Panel);
        } else {
            list_.draw(canvas_, focus_ == Focus::Panel);
        }
    }
    if (terminalVisible_) {
        drawTerminalWindow(g);
        terminal_.draw(canvas_, focus_ == Focus::Terminal && overlays_.empty() && hasKeyboardFocus());
    }
    drawStatusLine(g);
    if (suggestions_.visible()) {
        suggestions_.draw(canvas_);
    }
    if (hover_.visible()) {
        hover_.draw(canvas_);
    }
    if (changeBox_.visible()) {
        changeBox_.draw(canvas_);
    }
    for (auto& overlay : overlays_) {
        if (!overlay->closed()) {
            overlay->draw(canvas_);
        }
    }
}

void MainWindow::drawMenuBar(const Geometry& g) {
    const bitmap::Scheme& colors = Colors();
    canvas_.fill(g.menuBar, colors.bar);
    std::vector<ui::Rect> titles = menuTitleRects();
    int textTop = Snap(g.menuBar.y + g.menuBar.h / 2.0 - Line() / 2.0) + Pixel();
    static const char* const marked[] = {"≡", "&File", "&Edit", "&Search", "&Run", "&Debug", "&View", "&Options", "&Help"};
    for (size_t index = 0; index < titles.size(); ++index) {
        const ui::Rect& rect = titles[index];
        bool open = static_cast<int>(index) == openMenu_;
        if (open) {
            canvas_.fill({rect.x, textTop, rect.w, Line()}, colors.barHighlight);
        }
        if (index == 0) {
            ui::Rect size = ui::iconSize(ui::Icon::Menu, Pixel());
            ui::drawIconPlain(canvas_, ui::Icon::Menu, rect.x + Snap((rect.w - size.w) / 2.0), textTop + Snap((Line() - size.h) / 2.0),
                              Pixel(), colors.barHotkey);
            continue;
        }
        int hotkey = -1;
        std::string title = ui::withoutHotkeyMark(marked[index], &hotkey);
        ui::drawWithHotkey(canvas_, Chrome(), rect.x + Cell(), textTop, title, hotkey, open ? colors.barHighlightText : colors.barText,
                           colors.barHotkey, false);
    }
    std::vector<ui::Rect> buttons = windowButtonRects();
    int right = buttons.empty() ? g.menuBar.right() : buttons.front().x;
    std::string title = folder_.empty() ? std::string("Glasswork Bitmap") : utf8FromPath(pathFromUtf8(folder_).filename());
    int width = ui::textWidth(Chrome(), title);
    int x = Snap(right - 2 * Cell() - width);
    if (x > titles.back().right() + 2 * Cell()) {
        ui::drawTextPlain(canvas_, Chrome(), x, textTop, title, colors.barDim);
    }
    drawWindowButtons(textTop);
}

std::vector<ui::Rect> MainWindow::windowButtonRects() const {
    if (scripted_ && environmentValue("GLASSWORK_SCRIPT_BUTTONS").empty()) {
        return {};
    }
    int height = std::max(14 * Pixel(), Line());
    int width = 4 * Cell();
    int right = canvas_.width();
    return {{right - 3 * width, 0, width, height}, {right - 2 * width, 0, width, height}, {right - width, 0, width, height}};
}

void MainWindow::drawWindowButtons(int textTop) {
    const bitmap::Scheme& colors = Colors();
    std::vector<ui::Rect> buttons = windowButtonRects();
    int pixel = Pixel();
    bool maximized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
    for (size_t index = 0; index < buttons.size(); ++index) {
        const ui::Rect& box = buttons[index];
        bool hovered = static_cast<int>(index) == hoveredWindowButton_;
        Color ink = colors.barText;
        Color fill = colors.bar;
        if (hovered) {
            fill = index == 2 ? bitmap::kRed : colors.barHighlight;
            canvas_.fill({box.x, textTop, box.w, Line()}, fill);
            ink = index == 2 ? bitmap::kWhite : colors.barHighlightText;
        }
        // Brackets around a little picture in chrome pixels, like the code window's close box.
        int bracket = ui::textWidth(Chrome(), "[");
        ui::drawTextPlain(canvas_, Chrome(), box.x + Cell() / 2, textTop, "[", ink);
        ui::drawTextPlain(canvas_, Chrome(), box.right() - Cell() / 2 - bracket, textTop, "]", ink);
        int size = 5 * pixel;
        int left = box.x + (box.w - size) / 2 / pixel * pixel;
        int top = textTop + (Line() - size) / 2 / pixel * pixel;
        Color mark = hovered ? ink : (index == 2 ? colors.barHotkey : colors.barText);
        if (index == 0) {
            canvas_.fill({left, top + size - pixel, size, pixel}, mark);  // an underscore
        } else if (index == 1 && !maximized) {
            canvas_.frame({left, top, size, size}, mark, pixel);  // a window with its title strip
            canvas_.fill({left, top, size, 2 * pixel}, mark);
        } else if (index == 1) {
            // Two windows: back to the earlier size.
            canvas_.frame({left + pixel, top - pixel, size - pixel, size - pixel}, mark, pixel);
            canvas_.fill({left - pixel, top + pixel, size - pixel, size - pixel}, fill);
            canvas_.frame({left - pixel, top + pixel, size - pixel, size - pixel}, mark, pixel);
        } else {
            for (int step = 0; step < 5; ++step) {  // an X
                canvas_.fill({left + step * pixel, top + step * pixel, pixel, pixel}, mark);
                canvas_.fill({left + (4 - step) * pixel, top + step * pixel, pixel, pixel}, mark);
            }
        }
    }
}

void MainWindow::pressWindowButton(int index) {
    if (index == 0) {
        SDL_MinimizeWindow(window_);
    } else if (index == 1) {
        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) {
            SDL_RestoreWindow(window_);
        } else {
            SDL_MaximizeWindow(window_);
        }
    } else if (index == 2) {
        requestClose([this](bool ok) {
            if (ok) {
                app_.closeWindow(this);
            }
        });
    }
    hoveredWindowButton_ = -1;
    dirty_ = true;
}

SDL_HitTestResult MainWindow::hitTest(SDL_Window* window, const SDL_Point* area, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    int x = static_cast<int>(area->x * self->pixelDensity_), y = static_cast<int>(area->y * self->pixelDensity_);
    int width = self->canvas_.width(), height = self->canvas_.height();
    bool maximized = (SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0;
    if (!maximized) {
        // The edges resize the window, as a frame would.
        int edge = static_cast<int>(6 * std::max(1.0f, SDL_GetWindowDisplayScale(window)));
        bool left = x < edge, right = x >= width - edge, top = y < edge, bottom = y >= height - edge;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
    }
    int barHeight = std::max(14 * Pixel(), Line());
    if (y >= barHeight || !self->overlays_.empty()) {
        return SDL_HITTEST_NORMAL;
    }
    for (const ui::Rect& rect : self->menuTitleRects()) {
        if (rect.contains(x, y)) {
            return SDL_HITTEST_NORMAL;
        }
    }
    for (const ui::Rect& rect : self->windowButtonRects()) {
        if (rect.contains(x, y)) {
            return SDL_HITTEST_NORMAL;
        }
    }
    return SDL_HITTEST_DRAGGABLE;  // the rest of the menu bar moves the window; a double click maximises it
}

void MainWindow::drawEditorWindow(const Geometry& g) {
    int cell = Cell(), line = Line();
    const ui::Rect& window = g.editorWindow;
    const bitmap::Scheme& colors = Colors();
    canvas_.fill(window.offset(Shadow(), Shadow()), ui::kBlackColor);
    canvas_.fill(window, colors.window);
    drawFrame(window, colors.frame, true);

    int x = g.closeBox.x;
    x += drawOnEdge("[", x, window.y, colors.text, colors.window, true);
    x += drawOnEdge("■", x, window.y, colors.closeBox, colors.window, true);
    drawOnEdge("]", x, window.y, colors.text, colors.window, true);
    x = g.zoomBox.x;
    x += drawOnEdge("[", x, window.y, colors.text, colors.window, true);
    x += drawOnEdge(panelOpen_ ? "→" : "←", x, window.y, colors.closeBox, colors.window, true);
    drawOnEdge("]", x, window.y, colors.text, colors.window, true);
    drawTabs(g.closeBox.right() + cell, g.zoomBox.x - cell, window.y);

    int bottomTop = window.bottom() - line;
    std::string position = positionLabel();
    if (!position.empty()) {
        drawOnEdge(position, window.x + 2 * cell, bottomTop, colors.text, colors.window, true);
    }
    std::string mode = modeLabel();
    if (!mode.empty()) {
        drawOnEdge(mode, modeLeft(window), bottomTop, colors.textAccent, colors.window, true);
    }
    drawScrollbar(g, true);
    if (!g.sideTrack.empty()) {
        drawScrollbar(g, false);
    }
}

void MainWindow::drawTabs(int start, int limit, int top) {
    const bitmap::Scheme& colors = Colors();
    int cell = Cell();
    std::vector<std::string> labels;
    std::vector<int> widths;
    for (auto& tab : tabs_) {
        labels.push_back(" " + tab->title() + (tab->dirty() ? "*" : "") + " ");
        widths.push_back(ui::textWidth(Chrome(), labels.back()));
    }
    int shift = 0;
    if (selected_ >= 0 && static_cast<size_t>(selected_) < tabs_.size()) {
        int right = start;
        for (long index = 0; index <= selected_; ++index) {
            right += widths[static_cast<size_t>(index)] + cell;
        }
        shift = std::max(0, right - cell - limit);
    }
    tabRects_.assign(tabs_.size(), ui::Rect());
    ui::Rect clip{start, top, std::max(0, limit - start), Line()};
    canvas_.pushClip(clip);
    int x = start - shift;
    for (size_t index = 0; index < tabs_.size(); ++index) {
        bool selected = static_cast<long>(index) == selected_;
        if (selected) {
            drawOnEdge(labels[index], x, top, colors.selectionText, colors.selection, false);
        } else {
            drawOnEdge(labels[index], x, top, colors.text, colors.window, true);
        }
        tabRects_[index] = ui::Rect{x, top, widths[index], Line()}.intersect(clip);
        x += widths[index] + cell;
    }
    canvas_.popClip();
}

ui::Rect MainWindow::thumbRect(const Geometry& g, bool vertical) const {
    const ui::Rect& track = vertical ? g.track : g.sideTrack;
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return {};
    }
    double document = vertical ? editor_.contentHeight() : editor_.contentWidth();
    double visible = vertical ? editor_.visibleHeight() : editor_.visibleWidth();
    double offset = vertical ? editor_.scrollY() : editor_.scrollX();
    int length = vertical ? track.h : track.w;
    if (document <= visible + 1 || length <= Cell()) {
        return {};
    }
    int size = std::max(Cell(), Snap(length * visible / document));
    int begin = Snap((length - size) * std::clamp(offset / (document - visible), 0.0, 1.0));
    return vertical ? ui::Rect{track.x, track.y + begin, track.w, size} : ui::Rect{track.x + begin, track.y, size, track.h};
}

void MainWindow::drawScrollbar(const Geometry& g, bool vertical) {
    const bitmap::Scheme& colors = Colors();
    int pixel = Pixel();
    canvas_.fillChecker(vertical ? g.track : g.sideTrack, colors.window, colors.scrollTrack, pixel);
    const ui::Rect& back = vertical ? g.upArrow : g.leftArrow;
    const ui::Rect& forward = vertical ? g.downArrow : g.rightArrow;
    canvas_.fillShadowed(back, colors.scrollButton, pixel);
    canvas_.fillShadowed(forward, colors.scrollButton, pixel);
    auto arrow = [&](ui::Icon icon, const ui::Rect& box) {
        ui::Rect size = ui::iconSize(icon, pixel);
        ui::drawIcon(canvas_, icon, box.x + Snap((box.w - size.w) / 2.0), box.y + Snap((box.h - size.h) / 2.0), pixel, colors.scrollArrow,
                     false);
    };
    arrow(vertical ? ui::Icon::Up : ui::Icon::Left, back);
    arrow(vertical ? ui::Icon::Down : ui::Icon::Right, forward);
    ui::Rect thumb = thumbRect(g, vertical);
    if (!thumb.empty()) {
        canvas_.fillShadowed(thumb, colors.scrollThumb, pixel);
    }
}

void MainWindow::drawEmptyHint(const ui::Rect& area) {
    std::vector<std::string> lines = {
        "No file open",
        "",
        "File > Open Folder   " + shortcutForCommand("workbench.action.files.openFolder"),
        "File > Quick Open    " + shortcutForCommand("workbench.action.quickOpen"),
        "Debug > Start        " + shortcutForCommand("workbench.action.debug.start"),
    };
    int width = 0;
    for (const std::string& text : lines) {
        width = std::max(width, ui::textWidth(Chrome(), text));
    }
    int x = Snap(area.x + area.w / 2.0 - width / 2.0);
    int y = Snap(area.y + area.h / 2.0 - static_cast<double>(lines.size()) * Line() / 2);
    for (size_t index = 0; index < lines.size(); ++index) {
        ui::drawText(canvas_, Chrome(), x, y, lines[index], index == 0 ? Colors().text : Colors().textDim, true);
        y += Line();
    }
}

void MainWindow::drawPanelWindow(const Geometry& g) {
    const bitmap::Scheme& colors = Colors();
    const ui::Rect& window = g.panelWindow;
    canvas_.fill(window.offset(Shadow(), Shadow()), ui::kBlackColor);
    canvas_.fill(window, colors.window);
    drawFrame(window, colors.frameDim, false);
    panelTabRects_.assign(3, ui::Rect());
    int x = window.x + Cell();
    for (int index = 0; index < 3; ++index) {
        std::string label = std::string(" ") + kPanelNames[index] + " ";
        bool selected = index == static_cast<int>(panel_);
        int width = selected ? drawOnEdge(label, x, window.y, colors.selectionText, colors.selection, false)
                             : drawOnEdge(label, x, window.y, colors.text, colors.window, false);
        panelTabRects_[static_cast<size_t>(index)] = {x, window.y, width, Line()};
        x += width;
    }
}

void MainWindow::drawTerminalWindow(const Geometry& g) {
    const bitmap::Scheme& colors = Colors();
    const ui::Rect& window = g.terminalWindow;
    canvas_.fill(window.offset(Shadow(), Shadow()), ui::kBlackColor);
    canvas_.fill(window, colors.window);
    drawFrame(window, colors.frameDim, false);
    drawOnEdge(" Terminal ", window.x + Cell(), window.y, colors.text, colors.window, true);
}

void MainWindow::drawStatusLine(const Geometry& g) {
    const bitmap::Scheme& colors = Colors();
    canvas_.fill(g.status, colors.bar);
    int x = Cell();
    hintRects_.clear();
    hintCommands_.clear();
    for (const Hint& hint : kHints) {
        std::string key = Services::shared().keybindings().shortcutFor(hint.command, keyContext()).empty()
                              ? std::string(hint.fallbackKey)
                              : compactShortcutText(Services::shared().keybindings().shortcutFor(hint.command, keyContext()));
        int start = x;
        x += ui::drawTextPlain(canvas_, Chrome(), x, g.status.y, key, colors.barHotkey);
        x += ui::drawTextPlain(canvas_, Chrome(), x, g.status.y, std::string(" ") + hint.label, colors.barText);
        hintRects_.push_back({start - Cell() / 2, g.status.y, x - start + Cell(), Line()});
        hintCommands_.push_back(hint.command);
        x += 2 * Cell();
    }
    canvas_.fill({x - Cell(), g.status.y + 2 * Pixel(), Pixel(), Line() - 4 * Pixel()}, colors.barText);
    std::string message = !temporaryMessage_.empty() ? temporaryMessage_ : message_;
    if (!message.empty()) {
        int width = ui::textWidth(Chrome(), message);
        int left = std::max(x, Snap(g.status.right() - Cell() - width));
        canvas_.pushClip({x, g.status.y, std::max(0, g.status.right() - Cell() - x), Line()});
        ui::drawTextPlain(canvas_, Chrome(), left, g.status.y, message, colors.barText);
        canvas_.popClip();
    }
}

void MainWindow::drawImageTab(Tab& tab, const ui::Rect& area) {
    const bitmap::Scheme& colors = Colors();
    canvas_.fill(area, colors.window);
    if (tab.imageWidth <= 0 || tab.imageHeight <= 0) {
        return;
    }
    canvas_.pushClip(area);
    int room = 2 * Cell();
    int availableWidth = std::max(1, area.w - room), availableHeight = std::max(1, area.h - room);
    const uint8_t* pixels = tab.pixels.data();
    int width = tab.imageWidth, height = tab.imageHeight, scale = 1;
    if (width > availableWidth || height > availableHeight) {
        double factor = std::min(static_cast<double>(availableWidth) / width, static_cast<double>(availableHeight) / height);
        int wanted = std::max(1, static_cast<int>(width * factor)), wantedHeight = std::max(1, static_cast<int>(height * factor));
        if (tab.scaledWidth != wanted || tab.scaledHeight != wantedHeight) {
            tab.scaled.assign(static_cast<size_t>(wanted) * wantedHeight * 4, 0);
            for (int y = 0; y < wantedHeight; ++y) {
                int sourceY = std::min(height - 1, static_cast<int>(y / factor));
                for (int x = 0; x < wanted; ++x) {
                    int sourceX = std::min(width - 1, static_cast<int>(x / factor));
                    std::copy_n(&tab.pixels[(static_cast<size_t>(sourceY) * width + sourceX) * 4], 4,
                                &tab.scaled[(static_cast<size_t>(y) * wanted + x) * 4]);
                }
            }
            tab.scaledWidth = wanted;
            tab.scaledHeight = wantedHeight;
        }
        pixels = tab.scaled.data();
        width = wanted;
        height = wantedHeight;
    } else {
        scale = std::max(1, std::min(availableWidth / width, availableHeight / height));
        scale = std::min(scale, 8);
    }
    int x = area.x + (area.w - width * scale) / 2, y = area.y + (area.h - height * scale) / 2;
    canvas_.fillChecker({x, y, width * scale, height * scale}, 0x808080FF, 0xA0A0A0FF, 4 * Pixel());
    canvas_.drawImage(pixels, width, height, x, y, scale);
    canvas_.popClip();
    std::string size = std::to_string(tab.imageWidth) + " x " + std::to_string(tab.imageHeight);
    ui::drawText(canvas_, Chrome(), area.x, area.bottom() - Line(), size, colors.textDim, true);
}

void MainWindow::drawPreviewTab(Tab& tab, const ui::Rect& area) {
    const bitmap::Scheme& colors = Colors();
    canvas_.fill(area, colors.window);
    std::string text;
    if (Tab* source = tabForPath(tab.previewOf)) {
        text = source->document->buffer().text();
    } else if (auto document = Document::open(pathFromUtf8(tab.previewOf))) {
        text = document->buffer().text();
    }
    struct Row {
        std::string text;
        Color color;
        int indent;
    };
    std::vector<Row> rows;
    int columns = std::max(10, area.w / Cell() - 2);
    auto add = [&](const std::string& line, Color color, int indent) {
        for (const std::string& piece : ui::wrapText(line, static_cast<size_t>(std::max(4, columns - indent)))) {
            rows.push_back({piece, color, indent});
        }
        if (line.empty()) {
            rows.push_back({"", color, indent});
        }
    };
    bool afterList = false;
    for (const markdown::Block& block : markdown::parse(text)) {
        if (afterList && block.kind != markdown::Block::Kind::ListItem) {
            rows.push_back({"", colors.text, 0});
        }
        afterList = block.kind == markdown::Block::Kind::ListItem;
        switch (block.kind) {
            case markdown::Block::Kind::Heading: {
                std::string title = markdown::plainText(block.spans);
                add(title, colors.textAccent, 0);
                if (block.level <= 2) {
                    add(std::string(std::min<size_t>(title.size(), static_cast<size_t>(columns)), block.level == 1 ? '=' : '-'),
                        colors.frameDim, 0);
                }
                break;
            }
            case markdown::Block::Kind::Paragraph:
                add(markdown::plainText(block.spans), colors.text, 0);
                break;
            case markdown::Block::Kind::ListItem: {
                std::string marker = block.ordered ? std::to_string(block.number) + ". " : "* ";
                add(marker + markdown::plainText(block.spans), colors.text, 2 * block.level + 1);
                break;
            }
            case markdown::Block::Kind::Quote:
                add("| " + markdown::plainText(block.spans), colors.textDim, 1);
                break;
            case markdown::Block::Kind::CodeBlock: {
                size_t start = 0;
                while (start <= block.code.size()) {
                    size_t end = block.code.find('\n', start);
                    std::string line = block.code.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    rows.push_back({line, colors.link, 2});
                    if (end == std::string::npos) {
                        break;
                    }
                    start = end + 1;
                }
                break;
            }
            case markdown::Block::Kind::Rule:
                add(std::string(static_cast<size_t>(columns), '-'), colors.frameDim, 0);
                break;
            case markdown::Block::Kind::Table:
                for (const auto& row : block.rows) {
                    std::string line;
                    for (const auto& cell : row) {
                        line += (line.empty() ? "" : " | ") + markdown::plainText(cell);
                    }
                    add(line, colors.text, 1);
                }
                break;
        }
        if (block.kind != markdown::Block::Kind::ListItem) {
            rows.push_back({"", colors.text, 0});
        }
    }
    int visible = area.h / Line();
    tab.scrollY = std::clamp(tab.scrollY, 0.0, static_cast<double>(std::max(0, static_cast<int>(rows.size()) - visible)));
    canvas_.pushClip(area);
    int y = area.y;
    for (size_t index = static_cast<size_t>(tab.scrollY); index < rows.size() && y < area.bottom(); ++index) {
        ui::drawText(canvas_, Chrome(), area.x + Cell() + rows[index].indent * Cell(), y, rows[index].text, rows[index].color, true);
        y += Line();
    }
    canvas_.popClip();
}

double MainWindow::present() {
    double current = now();
    if (editor_.tick(current)) {
        dirty_ = true;
    }
    if (dirty_ && texture_ && renderer_) {
        dirty_ = false;
        draw();
        SDL_UpdateTexture(texture_, nullptr, canvas_.data(), canvas_.width() * 4);
        SDL_RenderClear(renderer_);
        SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }
    double next = -1;
    double editorFrame = editor_.nextFrameTime();
    if (editorFrame > 0) {
        next = std::max(0.0, editorFrame - now());
    }
    // Carets in text fields blink.
    bool fieldCaret = hasKeyboardFocus() &&
                      ((topOverlay() && topOverlay()->wantsText()) || focus_ == Focus::Find || focus_ == Focus::Terminal ||
                       (focus_ == Focus::Panel && debugPanel_->wantsKeys()));
    if (fieldCaret) {
        double blink = 0.53;
        next = next < 0 ? blink : std::min(next, blink);
        if (next >= blink) {
            dirty_ = true;  // the next wake draws the other half of the blink
        }
    }
    return next;
}

// ---------------------------------------------------------------------------
// Overlays and focus

void MainWindow::pushOverlay(std::unique_ptr<Overlay> overlay) {
    overlay->layout(canvas_.bounds());
    overlays_.push_back(std::move(overlay));
    suggestions_.hide();
    hover_.hide();
    dirty_ = true;
}

Overlay* MainWindow::topOverlay() const {
    for (auto overlay = overlays_.rbegin(); overlay != overlays_.rend(); ++overlay) {
        if (!(*overlay)->closed()) {
            return overlay->get();
        }
    }
    return nullptr;
}

void MainWindow::dropClosedOverlays() {
    size_t before = overlays_.size();
    overlays_.erase(std::remove_if(overlays_.begin(), overlays_.end(), [](const auto& overlay) { return overlay->closed(); }),
                    overlays_.end());
    if (overlays_.size() != before) {
        bool menuOpen = false;
        for (auto& overlay : overlays_) {
            if (auto* menu = dynamic_cast<MenuOverlay*>(overlay.get())) {
                menuOpen = menuOpen || menu->menuIndex() >= 0;
            }
        }
        if (!menuOpen) {
            openMenu_ = -1;
        }
        dirty_ = true;
    }
}

void MainWindow::setFocus(Focus focus) {
    if (focus_ == focus) {
        return;
    }
    Focus before = focus_;
    focus_ = focus;
    if (before == Focus::Editor) {
        editor_.focusChanged(false);
    }
    if (before == Focus::Terminal) {
        terminal_.focusChanged(false);
    }
    if (before == Focus::Panel) {
        debugPanel_->blur();
    }
    if (focus == Focus::Editor) {
        editor_.focusChanged(true);
    }
    if (focus == Focus::Terminal) {
        terminal_.focusChanged(true);
    }
    if (focus != Focus::Editor) {
        suggestions_.hide();
    }
    dirty_ = true;
}

void MainWindow::updateTextInput() {
    if (Tab* tab = currentTab(); tab && focus_ == Focus::Editor && tab->kind == Tab::Kind::Text) {
        ui::Rect caret = editor_.rectForPosition(tab->document->selection.caret);
        SDL_Rect area{static_cast<int>(caret.x / pixelDensity_), static_cast<int>(caret.y / pixelDensity_),
                      static_cast<int>(std::max(1, caret.w) / pixelDensity_), static_cast<int>(caret.h / pixelDensity_)};
        SDL_SetTextInputArea(window_, &area, 0);
    }
}

// ---------------------------------------------------------------------------
// Events

void MainWindow::handleEvent(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            updateScale();
            layout();
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            dirty_ = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            Services::shared().checkFilesForChanges();
            for (auto& tab : tabs_) {
                checkDiskChange(*tab);
            }
            if (focus_ == Focus::Editor) {
                editor_.focusChanged(true);
            }
            dirty_ = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            editor_.focusChanged(false);
            altAlone_ = false;
            pendingChords_.clear();
            if (Services::shared().settings().autoSave) {
                saveAllQuietly();
            }
            dirty_ = true;
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            editor_.mouseExited();
            hoveredWindowButton_ = -1;
            dirty_ = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            requestClose([this](bool ok) {
                if (ok) {
                    app_.closeWindow(this);
                }
            });
            break;
        case SDL_EVENT_KEY_DOWN:
            keyEvent(event.key, true);
            break;
        case SDL_EVENT_KEY_UP:
            keyEvent(event.key, false);
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (event.text.text) {
                textEvent(event.text.text);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            mouseButton(event.button, true);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            mouseButton(event.button, false);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            mouseMotion(event.motion);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            mouseWheel(event.wheel);
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data) {
                std::string path = event.drop.data;
                std::error_code error;
                if (std::filesystem::is_directory(pathFromUtf8(path), error)) {
                    if (folder_.empty()) {
                        openFolder(path);
                    } else {
                        app_.openWindow(path, {});
                    }
                } else {
                    openPath(path);
                }
            }
            break;
        default:
            break;
    }
    dropClosedOverlays();
}

void MainWindow::keyEvent(const SDL_KeyboardEvent& event, bool down) {
    SDL_Keymod mods = event.mod;
    bool isAlt = event.key == SDLK_LALT || event.key == SDLK_RALT;
    if (!down) {
        // Alt pressed and let go on its own opens the menu bar, as on Windows.
        if (isAlt && altAlone_ && overlays_.empty() && focus_ != Focus::Terminal && !(mods & SDL_KMOD_CTRL)) {
            openMenu(1);
        }
        altAlone_ = false;
        return;
    }
    if (isAlt) {
        altAlone_ = !event.repeat && !(mods & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT));
        return;
    }
    altAlone_ = false;
    KeyPress key;
    key.key = keyName(event.key, &key.keypad);
    key.control = (mods & SDL_KMOD_CTRL) != 0;
    key.shift = (mods & SDL_KMOD_SHIFT) != 0;
    key.alt = (mods & SDL_KMOD_ALT) != 0;
    key.meta = (mods & SDL_KMOD_GUI) != 0;
    key.repeat = event.repeat;
    if (key.key.empty()) {
        return;
    }
    suppressText_ = false;
    if (Overlay* overlay = topOverlay()) {
        overlay->keyDown(key);
        suppressText_ = !overlay->wantsText() || key.control || overlay->closed();
        // A menu opened by Alt+letter mustn't get the letter typed into the editor.
        dirty_ = true;
        return;
    }
    if (dispatchKey(key)) {
        suppressText_ = true;
        dirty_ = true;
        return;
    }
    dirty_ = true;
}

void MainWindow::textEvent(const std::string& text) {
    if (suppressText_) {
        suppressText_ = false;
        return;
    }
    if (Overlay* overlay = topOverlay()) {
        overlay->textInput(text);
        dirty_ = true;
        return;
    }
    Tab* tab = currentTab();
    switch (focus_) {
        case Focus::Editor:
            if (tab && tab->kind == Tab::Kind::Text) {
                editor_.textInput(text);
                updateTextInput();
            }
            break;
        case Focus::Terminal:
            terminal_.textInput(text);
            break;
        case Focus::Find:
            findBar_->textInput(text);
            break;
        case Focus::Panel:
            if (panel_ == SidePanel::Debug && debugPanel_->wantsKeys()) {
                debugPanel_->textInput(text);
            }
            break;
    }
    dirty_ = true;
}

void MainWindow::mouseButton(const SDL_MouseButtonEvent& event, bool down) {
    MouseEvent mouse;
    mouse.x = static_cast<int>(event.x * pixelDensity_);
    mouse.y = static_cast<int>(event.y * pixelDensity_);
    mouse.button = event.button;
    mouse.clicks = event.clicks;
    SDL_Keymod mods = SDL_GetModState();
    mouse.control = (mods & SDL_KMOD_CTRL) != 0;
    mouse.shift = (mods & SDL_KMOD_SHIFT) != 0;
    mouse.alt = (mods & SDL_KMOD_ALT) != 0;
    altAlone_ = false;
    dirty_ = true;
    if (!down) {
        Capture capture = capture_;
        capture_ = Capture::None;
        if (scrollTimer_ != 0) {
            cancelTimer(scrollTimer_);
            scrollTimer_ = 0;
        }
        switch (capture) {
            case Capture::Editor: editor_.mouseUp(mouse); break;
            case Capture::Terminal: terminal_.mouseUp(mouse); break;
            case Capture::Overlay:
                if (Overlay* overlay = topOverlay()) {
                    overlay->mouseUp(mouse);
                }
                break;
            case Capture::TerminalEdge:
                Services::shared().setValue("terminalLines", terminalHeight_ / Line());
                break;
            default: break;
        }
        return;
    }
    if (Overlay* overlay = topOverlay()) {
        if (overlay->mouseDown(mouse)) {
            capture_ = Capture::Overlay;
            return;
        }
    }
    if (changeBox_.visible()) {
        if (changeBox_.mouseDown(mouse)) {
            return;
        }
        changeBox_.hide();
    }
    if (suggestions_.visible() && suggestions_.contains(mouse.x, mouse.y)) {
        suggestions_.mouseDown(mouse);
        return;
    }
    suggestions_.hide();
    if (!hover_.contains(mouse.x, mouse.y)) {
        hover_.hide();
        signatureLine_ = -1;
    }
    if (chromeMouseDown(mouse)) {
        return;
    }
    Geometry g = geometry();
    Tab* tab = currentTab();
    if (findBar_->visible() && findBar_->frame().contains(mouse.x, mouse.y)) {
        setFocus(Focus::Find);
        findBar_->mouseDown(mouse);
        return;
    }
    if (g.editorContent.contains(mouse.x, mouse.y)) {
        setFocus(Focus::Editor);
        if (tab && tab->kind == Tab::Kind::Text) {
            if (editor_.mouseDown(mouse)) {
                capture_ = Capture::Editor;
            }
            updateTextInput();
        }
        return;
    }
    if (terminalVisible_ && g.terminalContent.contains(mouse.x, mouse.y)) {
        setFocus(Focus::Terminal);
        if (terminal_.mouseDown(mouse)) {
            capture_ = Capture::Terminal;
        }
        return;
    }
    if (panelOpen_ && g.panelContent.contains(mouse.x, mouse.y)) {
        setFocus(Focus::Panel);
        if (panel_ == SidePanel::Debug) {
            debugPanel_->mouseDown(mouse);
        } else {
            list_.mouseDown(mouse);
        }
        return;
    }
}

bool MainWindow::chromeMouseDown(const MouseEvent& mouse) {
    Geometry g = geometry();
    if (g.menuBar.contains(mouse.x, mouse.y)) {
        std::vector<ui::Rect> buttons = windowButtonRects();
        for (size_t index = 0; index < buttons.size(); ++index) {
            if (buttons[index].contains(mouse.x, mouse.y)) {
                pressWindowButton(static_cast<int>(index));
                return true;
            }
        }
        std::vector<ui::Rect> titles = menuTitleRects();
        for (size_t index = 0; index < titles.size(); ++index) {
            if (titles[index].contains(mouse.x, mouse.y)) {
                openMenu(static_cast<int>(index));
                return true;
            }
        }
        return true;
    }
    if (g.status.contains(mouse.x, mouse.y)) {
        for (size_t index = 0; index < hintRects_.size(); ++index) {
            if (hintRects_[index].contains(mouse.x, mouse.y)) {
                runCommand(hintCommands_[index]);
                return true;
            }
        }
        return true;
    }
    if (g.closeBox.contains(mouse.x, mouse.y)) {
        if (selected_ >= 0) {
            closeTab(selected_);
        }
        return true;
    }
    if (g.zoomBox.contains(mouse.x, mouse.y)) {
        toggleSidePanel();
        return true;
    }
    for (size_t index = 0; index < tabRects_.size() && index < tabs_.size(); ++index) {
        if (tabRects_[index].contains(mouse.x, mouse.y)) {
            if (mouse.button == 2) {
                closeTab(static_cast<long>(index));
            } else {
                showTab(static_cast<long>(index));
                setFocus(Focus::Editor);
            }
            return true;
        }
    }
    if (panelOpen_) {
        for (size_t index = 0; index < panelTabRects_.size(); ++index) {
            if (panelTabRects_[index].contains(mouse.x, mouse.y)) {
                openSidePanel(static_cast<SidePanel>(index));
                return true;
            }
        }
    }
    if (terminalVisible_ && mouse.y >= g.terminalWindow.y && mouse.y < g.terminalWindow.y + Line() && mouse.x >= g.terminalWindow.x &&
        mouse.x < g.terminalWindow.right()) {
        capture_ = Capture::TerminalEdge;
        return true;
    }
    if (currentTab() && currentTab()->kind == Tab::Kind::Text) {
        if (g.upArrow.unite(g.downArrow).contains(mouse.x, mouse.y)) {
            scrollbarMouseDown(mouse, true);
            return true;
        }
        if (!g.sideTrack.empty() && g.leftArrow.unite(g.rightArrow).contains(mouse.x, mouse.y)) {
            scrollbarMouseDown(mouse, false);
            return true;
        }
    }
    return false;
}

void MainWindow::scrollbarMouseDown(const MouseEvent& mouse, bool vertical) {
    Geometry g = geometry();
    scrollVertical_ = vertical;
    capture_ = Capture::Scrollbar;
    ui::Rect thumb = thumbRect(g, vertical);
    if (thumb.contains(mouse.x, mouse.y)) {
        draggingThumb_ = true;
        thumbGrab_ = vertical ? mouse.y - thumb.y : mouse.x - thumb.x;
        return;
    }
    draggingThumb_ = false;
    double line = 3.0 * editor_.lineHeight();
    double visible = vertical ? editor_.visibleHeight() : editor_.visibleWidth();
    const ui::Rect& back = vertical ? g.upArrow : g.leftArrow;
    const ui::Rect& forward = vertical ? g.downArrow : g.rightArrow;
    const ui::Rect& track = vertical ? g.track : g.sideTrack;
    int along = vertical ? mouse.y : mouse.x;
    if (back.contains(mouse.x, mouse.y)) {
        scrollStep_ = -line;
    } else if (forward.contains(mouse.x, mouse.y)) {
        scrollStep_ = line;
    } else if (track.contains(mouse.x, mouse.y)) {
        int thumbStart = vertical ? thumb.y : thumb.x;
        scrollStep_ = along < thumbStart ? -(visible - line) : visible - line;
    } else {
        scrollStep_ = 0;
        return;
    }
    scrollbarRepeat();
}

void MainWindow::scrollbarRepeat() {
    if (scrollVertical_) {
        editor_.scrollTo(editor_.scrollX(), editor_.scrollY() + scrollStep_);
    } else {
        editor_.scrollTo(editor_.scrollX() + scrollStep_, editor_.scrollY());
    }
    dirty_ = true;
    bool first = scrollTimer_ == 0;
    std::weak_ptr<bool> alive = alive_;
    scrollTimer_ = afterDelay(first ? 0.35 : 0.05, [this, alive] {
        if (!alive.expired() && capture_ == Capture::Scrollbar && !draggingThumb_) {
            scrollbarRepeat();
        }
    });
}

void MainWindow::mouseMotion(const SDL_MouseMotionEvent& event) {
    MouseEvent mouse;
    mouse.x = static_cast<int>(event.x * pixelDensity_);
    mouse.y = static_cast<int>(event.y * pixelDensity_);
    mouseX_ = mouse.x;
    mouseY_ = mouse.y;
    SDL_Keymod mods = SDL_GetModState();
    mouse.control = (mods & SDL_KMOD_CTRL) != 0;
    mouse.shift = (mods & SDL_KMOD_SHIFT) != 0;
    mouse.alt = (mods & SDL_KMOD_ALT) != 0;
    bool pressed = (event.state & SDL_BUTTON_LMASK) != 0;
    Geometry g = geometry();
    switch (capture_) {
        case Capture::Editor:
            editor_.mouseDragged(mouse);
            dirty_ = true;
            return;
        case Capture::Terminal:
            terminal_.mouseDragged(mouse);
            dirty_ = true;
            return;
        case Capture::Overlay:
            if (Overlay* overlay = topOverlay()) {
                overlay->mouseDragged(mouse);
                overlay->mouseMoved(mouse);
            }
            dirty_ = true;
            return;
        case Capture::TerminalEdge: {
            int bottom = g.status.y - Shadow() - Pixel();
            int wanted = Snap(bottom - mouse.y);
            terminalHeight_ = std::max(5 * Line(), wanted);
            layout();
            return;
        }
        case Capture::Scrollbar:
            if (draggingThumb_) {
                ui::Rect track = scrollVertical_ ? g.track : g.sideTrack;
                ui::Rect thumb = thumbRect(g, scrollVertical_);
                double room = (scrollVertical_ ? track.h - thumb.h : track.w - thumb.w);
                double offset = (scrollVertical_ ? mouse.y - track.y : mouse.x - track.x) - thumbGrab_;
                double document = scrollVertical_ ? editor_.contentHeight() : editor_.contentWidth();
                double visible = scrollVertical_ ? editor_.visibleHeight() : editor_.visibleWidth();
                double target = room > 0 ? std::clamp(offset / room, 0.0, 1.0) * (document - visible) : 0;
                if (scrollVertical_) {
                    editor_.scrollTo(editor_.scrollX(), target);
                } else {
                    editor_.scrollTo(target, editor_.scrollY());
                }
                dirty_ = true;
            }
            return;
        default:
            break;
    }
    int hovered = -1;
    std::vector<ui::Rect> buttons = windowButtonRects();
    for (size_t index = 0; index < buttons.size(); ++index) {
        if (buttons[index].contains(mouse.x, mouse.y)) {
            hovered = static_cast<int>(index);
        }
    }
    if (hovered != hoveredWindowButton_) {
        hoveredWindowButton_ = hovered;
        dirty_ = true;
    }
    if (Overlay* overlay = topOverlay()) {
        overlay->mouseMoved(mouse);
        dirty_ = true;
        return;
    }
    (void)pressed;
    // The pointer's shape says what a click would do.
    static SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    static SDL_Cursor* text = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    static SDL_Cursor* resize = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    SDL_Cursor* wanted = arrow;
    Tab* tab = currentTab();
    if (tab && tab->kind == Tab::Kind::Text && g.editorContent.contains(mouse.x, mouse.y) &&
        mouse.x >= g.editorContent.x + editor_.gutterWidth() && !(findBar_->visible() && findBar_->frame().contains(mouse.x, mouse.y))) {
        wanted = text;
    } else if (terminalVisible_ && g.terminalContent.contains(mouse.x, mouse.y)) {
        wanted = text;
    } else if (terminalVisible_ && mouse.y >= g.terminalWindow.y && mouse.y < g.terminalWindow.y + Line() &&
               mouse.x >= g.terminalWindow.x && mouse.x < g.terminalWindow.right()) {
        wanted = resize;
    }
    if (SDL_GetCursor() != wanted) {
        SDL_SetCursor(wanted);
    }
    if (tab && tab->kind == Tab::Kind::Text) {
        editor_.mouseMoved(mouse);
    }
}

void MainWindow::mouseWheel(const SDL_MouseWheelEvent& event) {
    WheelEvent wheel;
    wheel.x = static_cast<int>(event.mouse_x * pixelDensity_);
    wheel.y = static_cast<int>(event.mouse_y * pixelDensity_);
    double flip = event.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
    wheel.dx = event.x * flip;
    wheel.dy = event.y * flip;
    SDL_Keymod mods = SDL_GetModState();
    wheel.control = (mods & SDL_KMOD_CTRL) != 0;
    wheel.shift = (mods & SDL_KMOD_SHIFT) != 0;
    dirty_ = true;
    if (Overlay* overlay = topOverlay()) {
        overlay->wheel(wheel);
        return;
    }
    if (suggestions_.visible() && suggestions_.contains(wheel.x, wheel.y)) {
        suggestions_.wheel(wheel);
        return;
    }
    hover_.hide();
    if (wheel.control && wheel.dy != 0) {
        zoom(wheel.dy > 0 ? 1 : -1);
        return;
    }
    Geometry g = geometry();
    Tab* tab = currentTab();
    if (g.editorWindow.contains(wheel.x, wheel.y) && tab) {
        if (tab->kind == Tab::Kind::Preview) {
            tab->scrollY -= wheel.dy * 3;
            tab->scrollY = std::max(0.0, tab->scrollY);
        } else if (tab->kind == Tab::Kind::Text) {
            WheelEvent inside = wheel;
            inside.x = std::clamp(inside.x, g.editorContent.x, std::max(g.editorContent.x, g.editorContent.right() - 1));
            inside.y = std::clamp(inside.y, g.editorContent.y, std::max(g.editorContent.y, g.editorContent.bottom() - 1));
            editor_.wheel(inside);
        }
        return;
    }
    if (terminalVisible_ && g.terminalWindow.contains(wheel.x, wheel.y)) {
        terminal_.wheel(wheel);
        return;
    }
    if (panelOpen_ && g.panelWindow.contains(wheel.x, wheel.y)) {
        if (panel_ == SidePanel::Debug) {
            debugPanel_->wheel(wheel);
        } else {
            list_.wheel(wheel);
        }
    }
}

// ---------------------------------------------------------------------------
// Test scripts

void MainWindow::runScript(const std::string& path) {
    std::ifstream file(pathFromUtf8(path));
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            script_.push_back(line);
        }
    }
    scriptLine_ = 0;
    std::weak_ptr<bool> alive = alive_;
    afterDelay(0.3, [this, alive] {
        if (!alive.expired()) {
            runScriptStep();
        }
    });
}

void MainWindow::runScriptStep() {
    while (scriptLine_ < script_.size()) {
        std::string line = script_[scriptLine_++];
        size_t space = line.find(' ');
        std::string verb = line.substr(0, space);
        std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
        if (verb == "wait") {
            std::weak_ptr<bool> alive = alive_;
            afterDelay(std::atof(rest.c_str()), [this, alive] {
                if (!alive.expired()) {
                    runScriptStep();
                }
            });
            return;
        }
        if (verb == "key") {
            if (rest == "alt") {
                openMenu(1);
            } else {
                KeyPress key;
                size_t start = 0;
                while (true) {
                    size_t plus = rest.find('+', start);
                    std::string part = rest.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
                    if (plus == std::string::npos || plus + 1 == rest.size()) {
                        key.key = plus + 1 == rest.size() ? "+" : part;
                        break;
                    }
                    if (part == "ctrl") {
                        key.control = true;
                    } else if (part == "shift") {
                        key.shift = true;
                    } else if (part == "alt") {
                        key.alt = true;
                    } else if (part == "meta") {
                        key.meta = true;
                    }
                    start = plus + 1;
                }
                suppressText_ = false;
                if (Overlay* overlay = topOverlay()) {
                    overlay->keyDown(key);
                } else {
                    dispatchKey(key);
                }
            }
        } else if (verb == "type") {
            suppressText_ = false;
            textEvent(rest);
        } else if (verb == "click" || verb == "rightclick" || verb == "doubleclick") {
            int x = std::atoi(rest.c_str());
            int y = std::atoi(rest.c_str() + std::min(rest.size(), rest.find(' ') + 1));
            SDL_MouseButtonEvent event = {};
            event.x = static_cast<float>(x / pixelDensity_);
            event.y = static_cast<float>(y / pixelDensity_);
            event.button = verb == "rightclick" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
            event.clicks = verb == "doubleclick" ? 2 : 1;
            mouseButton(event, true);
            mouseButton(event, false);
        } else if (verb == "command") {
            runCommand(rest);
        } else if (verb == "open") {
            openPath(rest);
        } else if (verb == "shot") {
            layout();
            draw();
            dirty_ = true;
            ui::savePng(pathFromUtf8(rest), canvas_.data(), canvas_.width(), canvas_.height());
        } else if (verb == "quit") {
            for (auto& tab : tabs_) {
                if (tab->document) {
                    tab->document->onChange = nullptr;
                }
            }
            tabs_.clear();
            selected_ = -1;
            editor_.setDocument(nullptr, nullptr, LanguageConfiguration());
            app_.closeWindow(this);
            return;
        }
        dropClosedOverlays();
    }
}

}  // namespace gw::app
