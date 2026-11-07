#pragma once

#include "Canvas.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gw::ui {

/// One character, as a coverage mask placed relative to its cell's top left corner (the line top).
struct Glyph {
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    std::vector<uint8_t> mask;
};

/// A monospaced font at one size: every character takes one or two cells of `cellWidth` and the
/// lines are `lineHeight` apart. Characters the font lacks come from a fallback font, and box
/// drawing characters are drawn to fill their cells exactly, so terminal programs line up.
class Font {
public:
    virtual ~Font() = default;

    // ---- Metrics
    int cellWidth() const { return cellWidth_; }
    int lineHeight() const { return lineHeight_; }
    /// From the line's top to the baseline.
    int baseline() const { return baseline_; }
    /// One font pixel (the pixel font), or 1.
    int pixel() const { return pixel_; }
    /// The pixel font: hard edges, a one-pixel drop shadow the size of a font pixel.
    bool isPixelFont() const { return pixelFont_; }

    // ---- Glyphs
    const Glyph& glyph(char32_t character);
    /// Columns a character takes: 0 for combining marks, 2 for wide ones, else 1.
    static int cells(char32_t character);

protected:
    virtual Glyph render(char32_t character) = 0;
    /// The fallback fonts' (or box drawing's) picture of a character.
    Glyph fallback(char32_t character, bool crisp);

    int cellWidth_ = 6;
    int lineHeight_ = 12;
    int baseline_ = 9;
    int pixel_ = 1;
    bool pixelFont_ = false;

private:
    std::mutex mutex_;
    std::unordered_map<char32_t, Glyph> cache_;
};

// ---- Fonts
/// Glasswork's pixel font with every font pixel `pixel` device pixels square.
std::shared_ptr<Font> pixelFont(int pixel);
/// An installed monospaced font by family name at `pixelHeight` (its em size); null if not found.
std::shared_ptr<Font> trueTypeFont(const std::string& family, int pixelHeight);
/// Installed monospaced font families, sorted. Reads the font folders the first time (slow).
std::vector<std::string> monospaceFamilies();

// ---- Text (UTF-8)
char32_t decodeUtf8(std::string_view text, size_t& index);
std::string encodeUtf8(char32_t character);
/// Cells the text takes.
int textCells(std::string_view text);
/// Width in device pixels.
int textWidth(Font& font, std::string_view text);
/// Draws one line with its line box's top left at (x, y); `shadow` puts a black drop shadow
/// behind it (one font pixel right and down). Returns the width drawn.
int drawText(Canvas& canvas, Font& font, int x, int y, std::string_view text, Color color, bool shadow);
/// One character in its cell(s).
void drawGlyph(Canvas& canvas, Font& font, char32_t character, int x, int y, Color color, bool shadow);
/// Without the shadow: only for a character inside a block cursor, drawn in the background's color.
void drawGlyphPlain(Canvas& canvas, Font& font, char32_t character, int x, int y, Color color);
/// A line of text without the shadow: dark text on the grey menu bar, status line and side-window tabs.
int drawTextPlain(Canvas& canvas, Font& font, int x, int y, std::string_view text, Color color);

}  // namespace gw::ui
