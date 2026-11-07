#pragma once

#include "Canvas.hpp"
#include "Debugger.hpp"
#include "EditorConfig.hpp"
#include "EditorView.hpp"
#include "ExtensionHostService.hpp"
#include "FindBar.hpp"
#include "FolderWatcher.hpp"
#include "Highlighter.hpp"
#include "Input.hpp"
#include "LanguageService.hpp"
#include "LineDiff.hpp"
#include "NavigationHistory.hpp"
#include "Overlays.hpp"
#include "Popups.hpp"
#include "SidePanels.hpp"
#include "TerminalPanel.hpp"

#include <SDL3/SDL.h>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gw::app {

class App;

/// The side window's tabs.
enum class SidePanel { Files = 0, Issues = 1, Debug = 2 };

/// One open tab: a file (or an untitled one), a picture, or a Markdown preview.
struct Tab {
    enum class Kind { Text, Image, Preview };
    uint64_t identifier = 0;
    Kind kind = Kind::Text;
    std::unique_ptr<Document> document;
    std::unique_ptr<Highlighter> highlighter;
    LanguageConfiguration configuration;
    EditorConfigResult editorConfig;
    double scrollX = 0;
    double scrollY = 0;
    // A picture.
    std::vector<uint8_t> pixels;
    int imageWidth = 0;
    int imageHeight = 0;
    std::vector<uint8_t> scaled;  // the picture shrunk to fit, when it is bigger than the window
    int scaledWidth = 0;
    int scaledHeight = 0;
    // A preview: the file it shows.
    std::string previewOf;
    // Version control: the file as committed, and how the text differs from it.
    std::optional<std::string> headText;
    std::vector<DiffHunk> hunks;
    uint64_t diffTimer = 0;
    uint64_t autoSaveTimer = 0;
    std::map<std::string, std::vector<Decoration>> decorations;
    bool askedAboutDiskChange = false;

    std::string path() const;
    std::string title() const;
    bool dirty() const { return kind == Kind::Text && document && document->isDirty(); }
};

/// A Glasswork Bitmap window: the menu bar, the code window with its tabs, the side window (files,
/// issues, debugger), the terminal window and the status line, all drawn in pixels.
class MainWindow : public ExtensionHostDelegate {
public:
    MainWindow(App& app, const std::string& folder, const std::vector<std::string>& files);
    ~MainWindow() override;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // ---- The app's side
    SDL_WindowID windowId() const { return SDL_GetWindowID(window_); }
    SDL_Window* sdlWindow() const { return window_; }
    void handleEvent(const SDL_Event& event);
    /// Draws if something changed. Returns seconds until it wants another frame (negative: none).
    double present();
    /// Asks about unsaved files; `done(true)` when the window may close.
    void requestClose(std::function<void(bool)> done);
    bool wantsToClose() const { return closing_; }
    void openFolder(const std::string& folder);
    void openPath(const std::string& path, long line = 0, long column = 0);
    const std::string& folder() const { return folder_; }
    void needsDisplay() { dirty_ = true; }
    /// Test mode: runs the actions in a script file (keys, typing, clicks, screenshots) in a hidden window.
    void runScript(const std::string& path);

    // ---- ExtensionHostDelegate
    bool hostApplyWorkspaceEdit(const Json& edit) override;
    void hostOpenFile(const std::string& path, long line, long character) override;
    bool hostSaveFile(const std::string& path) override;
    void hostSaveAll() override;
    bool hostRunBuiltinCommand(const std::string& identifier, const Json& arguments) override;
    void hostSetSelections(const Json& selections, const std::string& path) override;
    void hostSetDecorations(const std::string& path, const std::string& key, const Json& options, const Json& ranges) override;
    void hostRevealRange(const Json& range, const std::string& path) override;
    bool hostInsertSnippet(const std::string& snippet, const Json& ranges, const std::string& path) override;
    void hostProvidersChanged() override;
    void hostSetContextKey(const std::string& key, const std::string& value) override;
    void hostShowStatusMessage(const std::string& text, double seconds) override;
    void hostStatusItemsChanged() override;
    void hostShowDialog(DialogOverlay::Options options, std::function<void(int button, const std::string& text)> done) override;
    void hostShowPalette(const std::string& placeholder, PaletteOverlay::Provider provider, std::function<void()> onDismiss) override;

private:
    enum class Focus { Editor, Terminal, Panel, Find };

    struct Geometry {
        ui::Rect menuBar, status;
        ui::Rect editorWindow, editorContent, closeBox, zoomBox;
        ui::Rect upArrow, downArrow, track, leftArrow, rightArrow, sideTrack;
        ui::Rect panelWindow, panelContent;
        ui::Rect terminalWindow, terminalContent;
    };

    // ---- MainWindow.cpp: window, layout, drawing, events
    void createWindow();
    void updateScale();
    Geometry geometry() const;
    std::vector<ui::Rect> menuTitleRects() const;
    void layout();
    void draw();
    void drawMenuBar(const Geometry& g);
    void drawEditorWindow(const Geometry& g);
    void drawTabs(int start, int limit, int top);
    void drawScrollbar(const Geometry& g, bool vertical);
    ui::Rect thumbRect(const Geometry& g, bool vertical) const;
    void drawEmptyHint(const ui::Rect& area);
    void drawPanelWindow(const Geometry& g);
    void drawTerminalWindow(const Geometry& g);
    void drawStatusLine(const Geometry& g);
    void drawImageTab(Tab& tab, const ui::Rect& area);
    void drawPreviewTab(Tab& tab, const ui::Rect& area);
    void drawFrame(const ui::Rect& rect, Color color, bool twoLines);
    int drawOnEdge(const std::string& text, int x, int top, Color color, Color background, bool shadow);
    std::string positionLabel() const;
    std::string modeLabel() const;
    int modeLeft(const ui::Rect& window) const;
    void keyEvent(const SDL_KeyboardEvent& event, bool down);
    void textEvent(const std::string& text);
    void mouseButton(const SDL_MouseButtonEvent& event, bool down);
    void mouseMotion(const SDL_MouseMotionEvent& event);
    void mouseWheel(const SDL_MouseWheelEvent& event);
    bool chromeMouseDown(const MouseEvent& event);
    void scrollbarMouseDown(const MouseEvent& event, bool vertical);
    void scrollbarRepeat();
    void setFocus(Focus focus);
    void pushOverlay(std::unique_ptr<Overlay> overlay);
    Overlay* topOverlay() const;
    void dropClosedOverlays();
    void updateTextInput();
    void focusEditor() { setFocus(Focus::Editor); }
    bool hasKeyboardFocus() const { return scripted_ || SDL_GetKeyboardFocus() == window_; }
    void runScriptStep();

    // ---- MainWindowCommands.cpp: commands, menus, keys
    void registerCommands();
    void addCommand(const std::string& id, const std::string& title, std::function<void()> run,
                    std::function<bool()> enabled = nullptr, bool hidden = false);
    bool runCommand(const std::string& id, const nlohmann::json& arguments = nullptr);
    std::map<std::string, std::string> keyContext() const;
    bool dispatchKey(const KeyPress& key);
    std::vector<Menu> buildMenus();
    std::vector<MenuItem> optionsItems();
    void openMenu(int index);
    MenuOverlay::Host menuHost();
    std::string shortcutForCommand(const std::string& id) const;
    void showCommandPalette();
    void showKeys();
    void showAbout();
    void chooseFont();
    void zoom(int steps);

    // ---- MainWindowFiles.cpp: tabs, files, folders, sessions
    Tab* currentTab() const;
    Tab* tabForPath(const std::string& path) const;
    Tab& addTab(std::unique_ptr<Tab> tab, bool select);
    void attachDocument(Tab& tab);
    void showTab(long index);
    void refresh();
    void closeTab(long index);
    void removeTab(long index);
    void closeOtherTabs();
    void reopenClosedTab();
    void newFile();
    void openFileDialog();
    void openFolderDialog(bool newWindow);
    bool saveTab(Tab& tab, std::function<void(bool)> done = nullptr);
    void saveTabAs(Tab& tab, std::function<void(bool)> done = nullptr);
    void writeTab(Tab& tab, std::function<void(bool)> done);
    void saveAll();
    void saveAllQuietly();
    void prepareForSaving(Tab& tab);
    void afterSave(Tab& tab);
    void documentEdited(Tab& tab);
    void scheduleAutoSave(Tab& tab);
    void openSettingsFile();
    void openKeybindingsFile();
    void showPreview();
    void openImage(const std::string& path);
    void filesChangedOnDisk(const std::vector<std::string>& paths);
    void checkDiskChange(Tab& tab);
    void closeTabsForFolderSwitch(std::function<void(bool)> done);
    void switchFolder(const std::string& folder);
    void saveSession();
    void restoreSession();
    void rebuildFileIndex();
    void quickOpen();
    void openRecent();
    void updateTitle();
    void reloadSidePanel();
    void reloadFiles();
    void reloadProblems();
    void chooseRow(const ListRow& row);
    void fileContextMenu(int row, int x, int y);
    void askForNewFile(bool folder);
    void renamePath(const std::string& path);
    void deletePath(const std::string& path);
    void openSidePanel(SidePanel panel);
    void toggleSidePanel();
    void showError(const std::string& title, const std::string& message);
    void showMessage(const std::string& title, const std::string& message);
    void setStatusMessage(const std::string& text, double seconds = 4);
    void updateStatus();
    void refreshGit(Tab& tab);
    void recomputeDiff(Tab& tab);
    void scheduleDiff(Tab& tab);
    void changeBarClicked(size_t line, ui::Rect bar);
    void jumpToChange(int direction);
    void rememberLocation();
    void navigate(bool back);
    void goToLine();
    void jumpToSection();
    std::string relativePath(const std::string& path) const;
    void applyEditorConfig(Tab& tab);
    void manageExtensions();
    void installExtensions();

    // ---- MainWindowCode.cpp: language features
    void setUpLanguage();
    void diagnosticsChanged(Document* document);
    void hoverAt(Position position, Range word);
    void editorTypedText(const std::optional<std::string>& typed);
    bool caretInWordStartingAt(Position start) const;
    Position wordStartBeforeCaret() const;
    std::string typedPrefix() const;
    void requestSuggestions(bool explicitly);
    std::vector<std::shared_ptr<Suggestion>> snippetSuggestions();
    void showSuggestions(const Json& result, Position wordStart);
    void showWordSuggestions(Position wordStart, bool explicitly);
    void presentSuggestions(std::vector<std::shared_ptr<Suggestion>> items, Position wordStart);
    void resolveSuggestion(std::shared_ptr<Suggestion> suggestion, Document* document, std::function<void()> then);
    void acceptSuggestion(std::shared_ptr<Suggestion> suggestion, const std::string& commitCharacter);
    void insertSuggestion(const Suggestion& suggestion);
    void applyEdits(std::vector<std::pair<Range, std::string>> edits, bool placeCaret);
    void requestSignatureHelp();
    void hideHint();
    Document* documentForLanguageCommand();
    void requestLocations(const std::string& method, const std::string& title, bool openSingle);
    void showLocations(const Json& result, const std::string& title, bool openSingle);
    void openLocation(const Json& location);
    void showPaletteItems(std::vector<PaletteItem> items, const std::string& placeholder);
    void showQuickFixes();
    bool codeActionCanRun(const Json& action, Document* document);
    void runCodeAction(const Json& action, Document* document, bool resolved);
    void executeServerCommand(const Json& command, Document* document);
    bool applyWorkspaceEdit(const Json& edit);
    void goToSymbolInFile();
    void goToSymbolInWorkspace();
    void renameSymbol();
    void formatDocument(Document* document, std::function<void()> after);
    void showProblemsList();
    void serverProgress(const std::string& server, const std::string& token, const std::string& kind, const std::string& title,
                        const std::string& message, int percentage);
    void scheduleEditorStateForExtensions();
    void sendEditorStateToExtensions();
    std::string runVimCommand(const std::string& command);
    void vimAction(VimAction action);

    // ---- MainWindowRun.cpp: terminal, tasks, debugging
    void setTerminalVisible(bool visible);
    void runTaskCommand(const std::string& command);
    void runTask();
    void buildProject();
    void runCurrentFile();
    void setUpDebugger();
    void startDebugging();
    void debugCurrentFile();
    void debugExecutable();
    void launchDebugProgram(const std::string& program, const std::vector<std::string>& arguments);
    void showDebugFrame(const DebugFrame& frame);
    void updateExecutionMarker();
    void breakpointsChanged();
    void toggleBreakpoint();
    void removeAllBreakpoints();
    std::map<std::string, std::set<size_t>> currentBreakpoints();
    void loadBreakpoints();
    nlohmann::json breakpointsJson();

    // ---- State
    App& app_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    ui::Canvas canvas_;
    double pixelDensity_ = 1;
    bool dirty_ = true;
    bool scripted_ = false;
    std::vector<std::string> script_;
    size_t scriptLine_ = 0;
    bool closing_ = false;
    int listener_ = 0;

    std::string folder_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    long selected_ = -1;
    uint64_t nextTabId_ = 1;
    std::vector<std::string> closedPaths_;
    std::vector<ui::Rect> tabRects_;
    std::vector<ui::Rect> panelTabRects_;
    std::vector<ui::Rect> hintRects_;
    std::vector<std::string> hintCommands_;

    EditorView editor_;
    std::unique_ptr<FindBar> findBar_;
    TerminalPanel terminal_;
    bool terminalVisible_ = false;
    int terminalHeight_ = 0;
    ListView list_;
    Debugger debugger_;
    std::unique_ptr<DebugPanel> debugPanel_;
    SidePanel panel_ = SidePanel::Files;
    bool panelOpen_ = true;
    std::string listedFolder_;
    std::string listedFile_;

    std::vector<std::unique_ptr<Overlay>> overlays_;
    int openMenu_ = -1;
    CompletionPopup suggestions_;
    HoverBox hover_;
    ChangeBox changeBox_;
    size_t changeLine_ = 0;
    Focus focus_ = Focus::Editor;

    // Keys.
    std::map<std::string, Command> commands_;
    std::vector<std::string> commandOrder_;
    std::vector<KeyChord> pendingChords_;
    bool altAlone_ = false;
    bool suppressText_ = false;
    std::map<std::string, std::string> extensionContext_;

    // Mouse.
    enum class Capture { None, Editor, Terminal, Panel, Overlay, Scrollbar, TerminalEdge };
    Capture capture_ = Capture::None;
    bool scrollVertical_ = true;
    bool draggingThumb_ = false;
    int thumbGrab_ = 0;
    double scrollStep_ = 0;
    uint64_t scrollTimer_ = 0;
    int mouseX_ = 0;
    int mouseY_ = 0;

    // Status.
    std::string message_;
    std::string temporaryMessage_;
    uint64_t messageTimer_ = 0;
    std::map<std::string, std::string> serverProgress_;
    std::string extensionMessage_;

    // Language features.
    std::unique_ptr<LanguageService> language_;
    std::unique_ptr<ExtensionHostService> extensionHost_;
    uint64_t hoverToken_ = 0;
    uint64_t suggestionToken_ = 0;
    uint64_t resolveToken_ = 0;
    bool suggestionsIncomplete_ = false;
    Position suggestionWordStart_;
    long signatureLine_ = -1;
    uint64_t editorStateTimer_ = 0;

    // Files.
    std::unique_ptr<FolderWatcher> watcher_;
    std::shared_ptr<std::vector<std::string>> fileIndex_;
    uint64_t indexGeneration_ = 0;
    std::string gitRoot_;
    NavigationHistory navigation_;
    bool navigating_ = false;

    // Debugging.
    std::map<std::string, std::set<size_t>> breakpointStore_;
    std::string executionPath_;
    long executionLine_ = -1;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace gw::app
