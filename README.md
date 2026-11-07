# Glasswork Bitmap

A code editor in the style of the old DOS IDEs, for **Windows** and **Linux**: a pixel font drawn sharp with a hard black shadow, yellow code on EGA blue, a grey menu bar whose menus drop down inside the window, the code in a double-framed window with its tabs set into the top edge, a side window for files, issues and the debugger, and a status line of keys you can click.

It started life as the bitmap mode of [Glasswork](https://github.com/natkon20043/Glasswork), a macOS editor. This repository is that bitmap version on its own, rebuilt to run everywhere: the same C++20 core (text engine, TextMate highlighting, Vim, terminal emulator, language servers, debug adapters, git, VS Code extensions) under a new front end that draws every pixel itself on top of SDL3 instead of AppKit.

Everything is native: no Electron, no web view. A single `glasswork-bitmap.exe` on Windows needs no DLLs beyond the system's.

## Install

### Windows

Build (below), then from the repository folder:

```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\install.ps1
```

This copies the program to `%LOCALAPPDATA%\Programs\Glasswork Bitmap`, adds it to the Start menu, lets `glasswork-bitmap` run from Win+R, adds "Open with Glasswork Bitmap" to folders' right-click menu (leave it out with `-NoContextMenu`) and registers an uninstaller in Settings > Apps. `packaging\windows\uninstall.ps1` removes it all again (your settings stay in `%APPDATA%\Glasswork Bitmap`).

### Linux

```sh
packaging/linux/install.sh
```

builds a release and installs it for your user: the program in `~/.local/share/glasswork-bitmap`, a `glasswork-bitmap` command in `~/.local/bin`, and a menu entry with its icon.

## Build

The first configuration downloads nlohmann/json and Oniguruma (and SDL3 if it isn't installed).

**Windows** with [MSYS2](https://www.msys2.org) (UCRT64):

```sh
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,sdl3}
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build/glasswork-bitmap.exe
```

The C++ runtime and SDL are linked in statically, so `build/glasswork-bitmap.exe` (with the `host` folder beside it) runs on any Windows 10 or 11 machine.

**Linux** (Fedora shown; Debian/Ubuntu have the same packages with `-dev`):

```sh
sudo dnf install gcc-c++ cmake ninja-build SDL3-devel libcurl-devel
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build/glasswork-bitmap
```

Under WSL, configure with a plain Linux `PATH` (`PATH=/usr/bin:/bin cmake ...`) so CMake doesn't pick up Windows packages through `/mnt/c`.

## Using it

Run `glasswork-bitmap [folder] [files...]`. With no arguments it reopens the folders you had open last time.

- **Menus**: click them, press Alt alone (then the arrows), or Alt and the red letter. ≡ File Edit Search Run Debug View Options Help, like Turbo C++.
- **Vim is always on**: `i a o` insert, `Esc` (or `Ctrl+[`) back to normal, `v V` visual, `hjkl w b`, `x dd`, `yy p` (the system clipboard), `u Ctrl+R`, `/ n N` search through the find bar, `:w :q :wq :x :q! :wa :qa :e file`, `:42` goes to a line, `:s/a/b/g` and `:%s` substitute. Help > Keys lists them.
- **Keys** follow VS Code on Windows and Linux: Ctrl+P open a file, Ctrl+Shift+P commands, Ctrl+S save, Ctrl+F / Ctrl+H find and replace, Ctrl+G go to line, Ctrl+6 sections, Ctrl+Shift+O symbols, Ctrl+T symbols in the project, F12 definition, Shift+F12 references, F2 rename, Ctrl+. quick fix, Shift+Alt+F format, Ctrl+Space suggestions, Ctrl+` terminal, Ctrl+B side window, Ctrl+Shift+E files, Ctrl+Shift+M issues, Ctrl+Shift+D debugger, F5 / Shift+F5 / F9 / F10 / F11 / Shift+F11 debugging, Ctrl+F5 run the file, Ctrl+Shift+B build, Ctrl+Alt+B tasks, Ctrl+= / Ctrl+- text size, Ctrl+Shift+V Markdown preview, Alt+Left / Alt+Right back and forward.
- **Your own keys**: Options > Keyboard... opens `keybindings.json` in VS Code's format (`{ "key": "ctrl+shift+u", "command": "editor.action.transformToUppercase", "when": "editorTextFocus" }`). Sequences (`ctrl+k ctrl+s`), `args`, `-command` removals and `when` clauses (`editorTextFocus`, `terminalFocus`, `inputFocus`, `editorLangId`, `resourceExtname`, `resourceFilename`, `editorHasSelection`...) work; extensions' `contributes.keybindings` (their `win`/`linux` keys first) are read too. Saving the file applies it.
- **Side window**: Files (a flat folder listing with `..` and `+ new`; right-click for new file/folder, rename, delete to the Recycle Bin or Trash, copy path, reveal, open a terminal there), Issues (the open file's problems), Debug (call stack, variables, a console that evaluates expressions). The `[→]` box in the code window's top edge hides it, `[■]` closes the tab.
- **Terminal**: PowerShell (or cmd) through ConPTY on Windows, your login shell through a pty on Linux; tabs, scrollback, mouse reporting, bracketed paste, Ctrl+click on `file:line:col` (and MSVC's `file(line,col)`) opens it.
- **Running and debugging**: Run > Run File runs scripts (Python, JavaScript, PowerShell, shell, Ruby, Go...) and single C, C++ and Rust files in the terminal; Run > Build and Run Task find CMake, Cargo, Go, npm, .NET and Make tasks. F5 builds the C/C++ file in front with debug info and runs it under `lldb-dap` or GDB's own debug adapter (`gdb --interpreter=dap`, GDB 14 or newer), or debugs a program you choose.
- **Version control**: bars beside the line numbers mark lines changed since the last commit; click one to see the old lines and revert or stage that change.
- **Code intelligence**: hover, completion (with snippets), signature help, go to definition and references, symbols, rename, quick fixes, formatting (also on save), inlay hints, semantic highlighting and problems as you type, from language servers: clangd, pyright, typescript-language-server, rust-analyzer, gopls and others when installed (on the PATH, or with npm into the `language-servers` folder in the settings folder, e.g. `npm install --prefix "%APPDATA%\Glasswork Bitmap\language-servers" typescript-language-server typescript@6` for JavaScript and TypeScript). Unused code is drawn faded and deprecated code struck through, as in VS Code. When a file's server is missing, the status line and the Issues list say how to install it. Without one, completion offers the words in the file.
- **Extensions**: the first start installs grammars for about thirty languages from [Open VSX](https://open-vsx.org); Options > Extensions... installs, disables and removes others. Extensions with code run in a Node.js extension host (needs `node` on the PATH, or a `node` folder beside the program), so their completions, hovers, formatters, commands and decorations work too.
- **Options**: three color schemes (Turbo, Low Tide, Phosphor; `"bitmap.colors"`), the pixel font or any installed monospaced font (`"bitmap.font"`), and the Vim cursor's smear (`"editor.vimCursorSmear"`).

## Settings

Options > Settings... opens `settings.json` (JSON with comments), which applies as you save. It lives in `%APPDATA%\Glasswork Bitmap` on Windows and `~/.config/glasswork-bitmap` on Linux (`$XDG_CONFIG_HOME` is honoured; `GLASSWORK_SUPPORT_FOLDER` overrides both). Keys include `editor.fontSize`, `editor.tabSize`, `editor.wordWrap`, `files.autoSave`, `files.formatOnSave`, `files.trimTrailingWhitespace`, `files.insertFinalNewline`, `terminal.shell`, `terminal.fontSize`, `lsp.enabled`, `bitmap.colors` and `bitmap.font`. `.editorconfig` files are honoured.

## Layout of the code

- `src/core`: the editor's engine in plain C++20, tested without any UI (`tests/`). Platform code is split into `ProcessWindows.cpp` / `ProcessPosix.cpp`, `PtyWindows.cpp` (ConPTY) / `PtyPosix.cpp` (forkpty), `Http.cpp` (WinHTTP / libcurl) and `Paths.cpp`.
- `src/ui`: a frame buffer, the pixel and TrueType fonts (stb_truetype) with box-drawing characters drawn to fill their cells, the pixel icons.
- `src/app`: the window and everything in it: `MainWindow*.cpp` (chrome, menus, commands, files, language features, running and debugging), `EditorView`, `TerminalPanel`, `FindBar`, `SidePanels`, `Popups`, `Overlays` (menus, dialogs, the palette), `LanguageService`, `Debugger`, `ExtensionHostService`, `FolderWatcher`.
- `host/`: the Node.js extension host that runs VS Code extensions.

## Credits

The look comes from the original Glasswork Bitmap:

From **[Picotron](https://www.lexaloffle.com/picotron.php)** by Lexaloffle: the look of its font (chunky glyphs five pixels wide, redrawn by hand in `core/PixelFont.cpp`; Picotron's font files weren't used), the one-pixel black shadow behind light text, and the side window's flat file list with a `+ new` row.

From **the DOS IDEs**, Borland's Turbo C++ and Turbo Pascal (Turbo Vision) and Microsoft's QBasic, as shown in Julio Merino's [The IDEs we had 30 years ago... and we lost](https://blogsystem5.substack.com/p/the-ides-we-had-30-years-ago-and): the grey menu bar with ≡ and red hotkey letters, the drop-down menus with a green bar and hard shadows, the double-framed blue code window with the `[■]` close box and a zoom box, the single-framed inactive window, the grey dialogs with a white title strip, the status line of red key names, and the sixteen EGA colors.

From **[Ocean Low Tide](https://github.com/martinemde/ocean-low-tide)** by martinemde (after Material Ocean): the Low Tide scheme's colors.

Libraries: [SDL3](https://libsdl.org), [nlohmann/json](https://github.com/nlohmann/json), [Oniguruma](https://github.com/kkos/oniguruma), [stb_truetype and stb_image](https://github.com/nothings/stb).

## License

See [LICENSE](LICENSE).
