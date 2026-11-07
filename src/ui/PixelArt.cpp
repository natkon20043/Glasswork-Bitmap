#include "PixelArt.hpp"

#include <string_view>
#include <vector>

namespace gw::ui {

namespace {

using IconArt = std::vector<std::string_view>;

const IconArt& artFor(Icon icon) {
    static const IconArt menu = {"#########", ".........", ".........", "#########", ".........", ".........", "#########"};
    static const IconArt folder = {"####.....", "#########", "#########", "#########", "#########", "#########", "#########"};
    static const IconArt file = {"#######", "#.....#", "#.###.#", "#.....#", "#.###.#", "#.....#", "#.###.#", "#######"};
    static const IconArt plus = {"..###..", "..###..", "#######", "#######", "#######", "..###..", "..###.."};
    static const IconArt alert = {"...#...", "..###..", "..#.#..", ".##.##.", ".#####.", "##.#.##", "#######"};
    static const IconArt up = {"..#..", ".###.", "#####"};
    static const IconArt down = {"#####", ".###.", "..#.."};
    static const IconArt right = {"#..", "##.", "###", "##.", "#.."};
    static const IconArt left = {"..#", ".##", "###", ".##", "..#"};
    static const IconArt image = {"#######", "#.....#", "#.#...#", "#...#.#", "#..###.", "#.#####", "#######"};
    static const IconArt gear = {"..#.#..", ".#####.", "##...##", ".#...#.", "##...##", ".#####.", "..#.#.."};
    static const IconArt bug = {"#.....#", ".#.#.#.", "..###..", "#######", "..###..", ".#####.", "#.###.#"};
    switch (icon) {
        case Icon::Menu: return menu;
        case Icon::Folder: return folder;
        case Icon::File: return file;
        case Icon::Plus: return plus;
        case Icon::Alert: return alert;
        case Icon::Up: return up;
        case Icon::Down: return down;
        case Icon::Right: return right;
        case Icon::Left: return left;
        case Icon::Image: return image;
        case Icon::Gear: return gear;
        case Icon::Bug: return bug;
    }
    return menu;
}

void fillArt(Canvas& canvas, const IconArt& art, int x, int y, int pixel, Color color) {
    for (size_t row = 0; row < art.size(); ++row) {
        std::string_view line = art[row];
        size_t column = 0;
        while (column < line.size()) {
            if (line[column] != '#') {
                ++column;
                continue;
            }
            size_t start = column;
            while (column < line.size() && line[column] == '#') {
                ++column;
            }
            canvas.fill({x + static_cast<int>(start) * pixel, y + static_cast<int>(row) * pixel,
                         static_cast<int>(column - start) * pixel, pixel},
                        color);
        }
    }
}

}  // namespace

Rect iconSize(Icon icon, int pixel) {
    const IconArt& art = artFor(icon);
    size_t width = 0;
    for (std::string_view line : art) {
        width = std::max(width, line.size());
    }
    return {0, 0, static_cast<int>(width) * pixel, static_cast<int>(art.size()) * pixel};
}

void drawIcon(Canvas& canvas, Icon icon, int x, int y, int pixel, Color color, bool shadow) {
    (void)shadow;  // every icon casts the shadow
    const IconArt& art = artFor(icon);
    fillArt(canvas, art, x + pixel, y + pixel, pixel, kBlackColor);
    fillArt(canvas, art, x, y, pixel, color);
}

void drawRetroBox(Canvas& canvas, const Rect& body, Color fill, Color line, int pixel, int shadow) {
    canvas.fill(body.offset(shadow, shadow), kBlackColor);
    canvas.fill(body, fill);
    canvas.frame(body.inset(2 * pixel, 2 * pixel), line, pixel);
}

std::vector<std::string> wrapText(std::string_view text, size_t columns) {
    std::vector<std::string> pieces;
    if (columns == 0 || static_cast<size_t>(textCells(text)) <= columns) {
        pieces.emplace_back(text);
        return pieces;
    }
    std::string current;
    size_t currentCells = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t space = text.find(' ', start);
        if (space == std::string_view::npos) {
            space = text.size();
        }
        std::string_view word = text.substr(start, space - start);
        // Words longer than a row are cut.
        while (static_cast<size_t>(textCells(word)) > columns) {
            if (!current.empty()) {
                pieces.push_back(current);
                current.clear();
                currentCells = 0;
            }
            size_t index = 0, cells = 0;
            while (index < word.size()) {
                size_t next = index;
                char32_t character = decodeUtf8(word, next);
                int width = Font::cells(character);
                if (cells + width > columns) {
                    break;
                }
                cells += width;
                index = next;
            }
            pieces.emplace_back(word.substr(0, index));
            word.remove_prefix(index);
        }
        size_t wordCells = static_cast<size_t>(textCells(word));
        size_t needed = current.empty() ? wordCells : currentCells + 1 + wordCells;
        if (needed > columns && !current.empty()) {
            pieces.push_back(current);
            current = std::string(word);
            currentCells = wordCells;
        } else {
            if (!current.empty()) {
                current += ' ';
                ++currentCells;
            }
            current += word;
            currentCells += wordCells;
        }
        start = space + 1;
        if (space == text.size()) {
            break;
        }
    }
    pieces.push_back(current);
    return pieces;
}

int drawWithHotkey(Canvas& canvas, Font& font, int x, int y, std::string_view title, int hotkey, Color color, Color hotkeyColor) {
    int start = x;
    int index = 0;
    for (size_t offset = 0; offset < title.size(); ++index) {
        char32_t character = decodeUtf8(title, offset);
        drawGlyph(canvas, font, character, x, y, index == hotkey ? hotkeyColor : color, false);
        x += Font::cells(character) * font.cellWidth();
    }
    return x - start;
}

std::string withoutHotkeyMark(std::string_view marked, int* hotkey) {
    size_t mark = marked.find('&');
    if (mark == std::string_view::npos || mark + 1 >= marked.size()) {
        *hotkey = -1;
        return std::string(marked);
    }
    *hotkey = textCells(marked.substr(0, mark));
    return std::string(marked.substr(0, mark)) + std::string(marked.substr(mark + 1));
}

}  // namespace gw::ui
