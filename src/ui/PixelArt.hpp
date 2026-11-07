#pragma once

#include "Canvas.hpp"
#include "Font.hpp"

namespace gw::ui {

// Icon name -> the little picture drawIcon draws, one chrome pixel per dot.
enum class Icon { Menu, Folder, File, Plus, Alert, Up, Down, Right, Left, Image, Gear, Bug };

// ---- Icons
/// Size in device pixels with dots `pixel` square.
Rect iconSize(Icon icon, int pixel);
/// Top left at (x, y); `shadow` adds the black drop shadow.
void drawIcon(Canvas& canvas, Icon icon, int x, int y, int pixel, Color color, bool shadow);

// ---- DOS boxes
/// A flat box with a thin frame line two pixels inside its edge and a hard black shadow down and
/// to the right (`shadow` pixels), like the dialogs and menus of the old IDEs.
void drawRetroBox(Canvas& canvas, const Rect& body, Color fill, Color line, int pixel, int shadow);

/// `text` cut at spaces into pieces of at most `columns` characters (longer words are split).
std::vector<std::string> wrapText(std::string_view text, size_t columns);

/// "File > Open" style text with one letter (the hotkey, or -1 for none) in another color.
int drawWithHotkey(Canvas& canvas, Font& font, int x, int y, std::string_view title, int hotkey, Color color, Color hotkeyColor);

/// Splits "&File" into "File" and the hotkey's index (in characters), -1 if there is none.
std::string withoutHotkeyMark(std::string_view marked, int* hotkey);

}  // namespace gw::ui
