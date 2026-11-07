#include "Font.hpp"

#include "CharacterWidth.hpp"
#include "Paths.hpp"
#include "PixelFont.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <set>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace gw::ui {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// UTF-8

char32_t decodeUtf8(std::string_view text, size_t& index) {
    unsigned char first = static_cast<unsigned char>(text[index]);
    if (first < 0x80) {
        ++index;
        return first;
    }
    int extra = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC0 ? 1 : 0;
    if (extra == 0 || index + extra >= text.size()) {
        ++index;
        return 0xFFFD;
    }
    char32_t value = first & (0x3F >> extra);
    for (int step = 1; step <= extra; ++step) {
        unsigned char next = static_cast<unsigned char>(text[index + step]);
        if ((next & 0xC0) != 0x80) {
            index += step;
            return 0xFFFD;
        }
        value = (value << 6) | (next & 0x3F);
    }
    index += extra + 1;
    return value;
}

std::string encodeUtf8(char32_t character) {
    std::string out;
    if (character < 0x80) {
        out += static_cast<char>(character);
    } else if (character < 0x800) {
        out += static_cast<char>(0xC0 | (character >> 6));
        out += static_cast<char>(0x80 | (character & 0x3F));
    } else if (character < 0x10000) {
        out += static_cast<char>(0xE0 | (character >> 12));
        out += static_cast<char>(0x80 | ((character >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (character & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (character >> 18));
        out += static_cast<char>(0x80 | ((character >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((character >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (character & 0x3F));
    }
    return out;
}

int Font::cells(char32_t character) {
    if (character < 0x7F) {
        return 1;
    }
    return codePointCells(static_cast<uint32_t>(character));
}

int textCells(std::string_view text) {
    int total = 0;
    for (size_t index = 0; index < text.size();) {
        total += Font::cells(decodeUtf8(text, index));
    }
    return total;
}

int textWidth(Font& font, std::string_view text) {
    return textCells(text) * font.cellWidth();
}

void drawGlyph(Canvas& canvas, Font& font, char32_t character, int x, int y, Color color, bool shadow) {
    if (character == ' ' || character == '\t') {
        return;
    }
    const Glyph& glyph = font.glyph(character);
    if (glyph.mask.empty()) {
        return;
    }
    // The bitmap look: everything casts the hard black shadow, so the flag no longer turns it off.
    (void)shadow;
    int offset = font.pixel();
    canvas.blendMask(glyph.mask.data(), glyph.width, glyph.height, x + glyph.left + offset, y + glyph.top + offset, kBlackColor);
    canvas.blendMask(glyph.mask.data(), glyph.width, glyph.height, x + glyph.left, y + glyph.top, color);
}

void drawGlyphPlain(Canvas& canvas, Font& font, char32_t character, int x, int y, Color color) {
    if (character == ' ' || character == '\t') {
        return;
    }
    const Glyph& glyph = font.glyph(character);
    if (!glyph.mask.empty()) {
        canvas.blendMask(glyph.mask.data(), glyph.width, glyph.height, x + glyph.left, y + glyph.top, color);
    }
}

int drawTextPlain(Canvas& canvas, Font& font, int x, int y, std::string_view text, Color color) {
    int start = x;
    for (size_t index = 0; index < text.size();) {
        char32_t character = decodeUtf8(text, index);
        drawGlyphPlain(canvas, font, character, x, y, color);
        x += Font::cells(character) * font.cellWidth();
    }
    return x - start;
}

int drawText(Canvas& canvas, Font& font, int x, int y, std::string_view text, Color color, bool shadow) {
    int start = x;
    for (size_t index = 0; index < text.size();) {
        char32_t character = decodeUtf8(text, index);
        int cells = Font::cells(character);
        drawGlyph(canvas, font, character, x, y, color, shadow);
        x += cells * font.cellWidth();
    }
    return x - start;
}

// ---------------------------------------------------------------------------
// Font files

namespace {

struct FontFile {
    fs::path path;
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

std::shared_ptr<FontFile> loadFontFile(const fs::path& path) {
    auto file = std::make_shared<FontFile>();
    file->path = path;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return file;
    }
    file->data.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (file->data.size() < 12) {
        return file;
    }
    int offset = stbtt_GetFontOffsetForIndex(file->data.data(), 0);
    file->ok = offset >= 0 && stbtt_InitFont(&file->info, file->data.data(), offset) != 0;
    return file;
}

std::vector<fs::path> fontFolders() {
    std::vector<fs::path> folders;
#ifdef _WIN32
    std::string windows = environmentValue("WINDIR");
    folders.push_back(pathFromUtf8((windows.empty() ? std::string("C:\\Windows") : windows) + "\\Fonts"));
    std::string local = environmentValue("LOCALAPPDATA");
    if (!local.empty()) {
        folders.push_back(pathFromUtf8(local + "\\Microsoft\\Windows\\Fonts"));
    }
#else
    folders.push_back("/usr/share/fonts");
    folders.push_back("/usr/local/share/fonts");
    folders.push_back(fs::path(homeFolder()) / ".local/share/fonts");
    folders.push_back(fs::path(homeFolder()) / ".fonts");
#endif
    return folders;
}

bool isFontFile(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".ttf" || extension == ".otf" || extension == ".ttc";
}

std::vector<fs::path> allFontFiles() {
    std::vector<fs::path> files;
    std::error_code error;
    for (const fs::path& folder : fontFolders()) {
        if (!fs::is_directory(folder, error)) {
            continue;
        }
        for (auto entry = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, error);
             entry != fs::recursive_directory_iterator(); entry.increment(error)) {
            if (error) {
                break;
            }
            if (entry->is_regular_file(error) && isFontFile(entry->path())) {
                files.push_back(entry->path());
            }
        }
    }
    return files;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// Fonts tried, in order, for characters the main font lacks.
const std::vector<std::shared_ptr<FontFile>>& fallbackFonts() {
    static std::vector<std::shared_ptr<FontFile>> fonts = [] {
#ifdef _WIN32
        std::vector<std::string> wanted = {"consola.ttf", "seguisym.ttf", "segoeui.ttf", "msgothic.ttc", "yugothr.ttc",
                                           "malgun.ttf", "msyh.ttc", "simsun.ttc", "seguiemj.ttf", "arial.ttf"};
#else
        std::vector<std::string> wanted = {"dejavusansmono.ttf", "notosansmono-regular.ttf", "liberationmono-regular.ttf",
                                           "dejavusans.ttf", "notosanssymbols2-regular.ttf", "notosanssymbols-regular.ttf",
                                           "notosanscjk-regular.ttc", "notosanscjksc-regular.otf", "wqy-microhei.ttc",
                                           "droidsansfallbackfull.ttf", "notosans-regular.ttf"};
#endif
        std::map<std::string, fs::path> found;
#ifdef _WIN32
        for (const fs::path& folder : fontFolders()) {
            for (const std::string& name : wanted) {
                std::error_code error;
                fs::path candidate = folder / name;
                if (!found.count(name) && fs::exists(candidate, error)) {
                    found[name] = candidate;
                }
            }
        }
#else
        for (const fs::path& path : allFontFiles()) {
            std::string name = lower(path.filename().string());
            if (std::find(wanted.begin(), wanted.end(), name) != wanted.end() && !found.count(name)) {
                found[name] = path;
            }
        }
#endif
        std::vector<std::shared_ptr<FontFile>> loaded;
        for (const std::string& name : wanted) {
            auto it = found.find(name);
            if (it == found.end()) {
                continue;
            }
            auto file = loadFontFile(it->second);
            if (file->ok) {
                loaded.push_back(file);
            }
        }
        return loaded;
    }();
    return fonts;
}

/// A font's name entry as UTF-8 (Windows names are UTF-16 big endian).
std::string nameOf(const stbtt_fontinfo& info, int nameId) {
    int length = 0;
    const char* name = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                               STBTT_MS_LANG_ENGLISH, nameId);
    if (name != nullptr && length > 0) {
        std::string text;
        for (int index = 0; index + 1 < length; index += 2) {
            char32_t unit = (static_cast<unsigned char>(name[index]) << 8) | static_cast<unsigned char>(name[index + 1]);
            text += encodeUtf8(unit);
        }
        return text;
    }
    name = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, nameId);
    if (name != nullptr && length > 0) {
        return std::string(name, static_cast<size_t>(length));
    }
    return std::string();
}

struct FamilyEntry {
    fs::path path;
    bool regular = false;
};

/// Monospaced families and the file of each one's upright regular face.
const std::map<std::string, FamilyEntry>& familyIndex() {
    static std::once_flag once;
    static std::map<std::string, FamilyEntry> index;
    std::call_once(once, [] {
        for (const fs::path& path : allFontFiles()) {
            std::error_code error;
            if (fs::file_size(path, error) > 40 * 1024 * 1024) {
                continue;
            }
            auto file = loadFontFile(path);
            if (!file->ok) {
                continue;
            }
            int narrow = 0, wide = 0, bearing = 0;
            int iGlyph = stbtt_FindGlyphIndex(&file->info, 'i');
            int wGlyph = stbtt_FindGlyphIndex(&file->info, 'W');
            if (iGlyph == 0 || wGlyph == 0) {
                continue;
            }
            stbtt_GetGlyphHMetrics(&file->info, iGlyph, &narrow, &bearing);
            stbtt_GetGlyphHMetrics(&file->info, wGlyph, &wide, &bearing);
            if (narrow != wide || narrow == 0) {
                continue;
            }
            std::string family = nameOf(file->info, 16);
            if (family.empty()) {
                family = nameOf(file->info, 1);
            }
            std::string style = lower(nameOf(file->info, 17));
            if (style.empty()) {
                style = lower(nameOf(file->info, 2));
            }
            if (family.empty()) {
                continue;
            }
            bool regular = style == "regular" || style == "book" || style == "roman" || style == "normal";
            auto existing = index.find(family);
            if (existing == index.end() || (regular && !existing->second.regular)) {
                index[family] = {path, regular};
            }
        }
    });
    return index;
}

// ---------------------------------------------------------------------------
// Box drawing: lines and blocks filling their cells exactly.

/// Which of the four arms of a box-drawing character are drawn: 0 none, 1 light, 2 heavy, 3 double.
struct BoxArms {
    int up = 0, down = 0, left = 0, right = 0;
};

std::optional<BoxArms> boxArms(char32_t c) {
    switch (c) {
        case 0x2500: return BoxArms{0, 0, 1, 1};
        case 0x2501: return BoxArms{0, 0, 2, 2};
        case 0x2502: return BoxArms{1, 1, 0, 0};
        case 0x2503: return BoxArms{2, 2, 0, 0};
        case 0x250C: return BoxArms{0, 1, 0, 1};
        case 0x250F: return BoxArms{0, 2, 0, 2};
        case 0x2510: return BoxArms{0, 1, 1, 0};
        case 0x2513: return BoxArms{0, 2, 2, 0};
        case 0x2514: return BoxArms{1, 0, 0, 1};
        case 0x2517: return BoxArms{2, 0, 0, 2};
        case 0x2518: return BoxArms{1, 0, 1, 0};
        case 0x251B: return BoxArms{2, 0, 2, 0};
        case 0x251C: return BoxArms{1, 1, 0, 1};
        case 0x2523: return BoxArms{2, 2, 0, 2};
        case 0x2524: return BoxArms{1, 1, 1, 0};
        case 0x252B: return BoxArms{2, 2, 2, 0};
        case 0x252C: return BoxArms{0, 1, 1, 1};
        case 0x2533: return BoxArms{0, 2, 2, 2};
        case 0x2534: return BoxArms{1, 0, 1, 1};
        case 0x253B: return BoxArms{2, 0, 2, 2};
        case 0x253C: return BoxArms{1, 1, 1, 1};
        case 0x254B: return BoxArms{2, 2, 2, 2};
        case 0x2550: return BoxArms{0, 0, 3, 3};
        case 0x2551: return BoxArms{3, 3, 0, 0};
        case 0x2554: return BoxArms{0, 3, 0, 3};
        case 0x2557: return BoxArms{0, 3, 3, 0};
        case 0x255A: return BoxArms{3, 0, 0, 3};
        case 0x255D: return BoxArms{3, 0, 3, 0};
        case 0x2560: return BoxArms{3, 3, 0, 3};
        case 0x2563: return BoxArms{3, 3, 3, 0};
        case 0x2566: return BoxArms{0, 3, 3, 3};
        case 0x2569: return BoxArms{3, 0, 3, 3};
        case 0x256C: return BoxArms{3, 3, 3, 3};
        case 0x256D: return BoxArms{0, 1, 0, 1};  // rounded corners drawn square
        case 0x256E: return BoxArms{0, 1, 1, 0};
        case 0x256F: return BoxArms{1, 0, 1, 0};
        case 0x2570: return BoxArms{1, 0, 0, 1};
        case 0x2574: return BoxArms{0, 0, 1, 0};
        case 0x2575: return BoxArms{1, 0, 0, 0};
        case 0x2576: return BoxArms{0, 0, 0, 1};
        case 0x2577: return BoxArms{0, 1, 0, 0};
        default: return std::nullopt;
    }
}

Glyph boxGlyph(char32_t c, int width, int height, int line) {
    Glyph glyph;
    glyph.width = width;
    glyph.height = height;
    glyph.mask.assign(static_cast<size_t>(width) * height, 0);
    auto fillRect = [&](int x0, int y0, int x1, int y1, uint8_t value = 255) {
        x0 = std::clamp(x0, 0, width);
        x1 = std::clamp(x1, 0, width);
        y0 = std::clamp(y0, 0, height);
        y1 = std::clamp(y1, 0, height);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                glyph.mask[static_cast<size_t>(y) * width + x] = value;
            }
        }
    };
    // Blocks and shades.
    if (c >= 0x2580 && c <= 0x259F) {
        int halfX = width / 2, halfY = height / 2;
        switch (c) {
            case 0x2580: fillRect(0, 0, width, halfY); break;
            case 0x2584: fillRect(0, halfY, width, height); break;
            case 0x2588: fillRect(0, 0, width, height); break;
            case 0x258C: fillRect(0, 0, halfX, height); break;
            case 0x2590: fillRect(halfX, 0, width, height); break;
            case 0x2591: fillRect(0, 0, width, height, 64); break;
            case 0x2592: fillRect(0, 0, width, height, 128); break;
            case 0x2593: fillRect(0, 0, width, height, 192); break;
            case 0x2596: fillRect(0, halfY, halfX, height); break;
            case 0x2597: fillRect(halfX, halfY, width, height); break;
            case 0x2598: fillRect(0, 0, halfX, halfY); break;
            case 0x259D: fillRect(halfX, 0, width, halfY); break;
            default:
                if (c >= 0x2581 && c <= 0x2587) {  // lower eighths
                    int eighths = static_cast<int>(c - 0x2580);
                    fillRect(0, height - height * eighths / 8, width, height);
                } else if (c >= 0x2589 && c <= 0x258F) {  // left eighths
                    int eighths = 8 - static_cast<int>(c - 0x2588);
                    fillRect(0, 0, width * eighths / 8, height);
                } else {
                    fillRect(0, 0, width, height, 128);
                }
                break;
        }
        return glyph;
    }
    BoxArms arms = *boxArms(c);
    int cx = width / 2, cy = height / 2;
    int heavy = line * 2;
    auto thickness = [&](int kind) { return kind == 2 ? heavy : line; };
    auto horizontal = [&](int x0, int x1, int kind) {
        if (kind == 3) {
            fillRect(x0, cy - line - line / 2, x1, cy - line / 2);
            fillRect(x0, cy + line - line / 2, x1, cy + 2 * line - line / 2);
        } else if (kind > 0) {
            int t = thickness(kind);
            fillRect(x0, cy - t / 2, x1, cy - t / 2 + t);
        }
    };
    auto vertical = [&](int y0, int y1, int kind) {
        if (kind == 3) {
            fillRect(cx - line - line / 2, y0, cx - line / 2, y1);
            fillRect(cx + line - line / 2, y0, cx + 2 * line - line / 2, y1);
        } else if (kind > 0) {
            int t = thickness(kind);
            fillRect(cx - t / 2, y0, cx - t / 2 + t, y1);
        }
    };
    int reach = std::max(line, heavy) + line;
    horizontal(0, arms.left ? cx + (arms.right ? 0 : reach / 2 + 1) : 0, arms.left);
    horizontal(arms.right ? cx - (arms.left ? 0 : reach / 2) : width, width, arms.right);
    vertical(0, arms.up ? cy + (arms.down ? 0 : reach / 2 + 1) : 0, arms.up);
    vertical(arms.down ? cy - (arms.up ? 0 : reach / 2) : height, height, arms.down);
    return glyph;
}

// ---------------------------------------------------------------------------
// The pixel font

class PixelFace : public Font {
public:
    explicit PixelFace(int pixel) {
        pixel_ = std::max(1, pixel);
        pixelFont_ = true;
        cellWidth_ = pixelfont::kAdvance * pixel_;
        lineHeight_ = pixelfont::kLineHeight * pixel_;
        baseline_ = pixelfont::kBaseline * pixel_;
    }

protected:
    Glyph render(char32_t character) override {
        const pixelfont::Glyph* art = pixelfont::glyph(character);
        if (art == nullptr) {
            return fallback(character, true);
        }
        Glyph glyph;
        glyph.width = pixelfont::kGlyphWidth * pixel_;
        glyph.height = pixelfont::kGlyphRows * pixel_;
        glyph.left = 0;
        glyph.top = (pixelfont::kBaseline - pixelfont::kRowsAboveBaseline) * pixel_;
        glyph.mask.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
        for (int row = 0; row < pixelfont::kGlyphRows; ++row) {
            for (int column = 0; column < pixelfont::kGlyphWidth; ++column) {
                if (((*art)[row] >> (pixelfont::kGlyphWidth - 1 - column)) & 1) {
                    for (int y = 0; y < pixel_; ++y) {
                        uint8_t* line = glyph.mask.data() + static_cast<size_t>(row * pixel_ + y) * glyph.width + column * pixel_;
                        std::fill(line, line + pixel_, 255);
                    }
                }
            }
        }
        return glyph;
    }
};

// ---------------------------------------------------------------------------
// TrueType fonts

class TrueTypeFace : public Font {
public:
    TrueTypeFace(std::shared_ptr<FontFile> file, int pixelHeight) : file_(std::move(file)) {
        pixel_ = 1;
        scale_ = stbtt_ScaleForMappingEmToPixels(&file_->info, static_cast<float>(pixelHeight));
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&file_->info, &ascent, &descent, &gap);
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&file_->info, 'M', &advance, &bearing);
        cellWidth_ = std::max(1, static_cast<int>(std::lround(advance * scale_)));
        double natural = (ascent - descent + gap) * scale_;
        lineHeight_ = std::max(static_cast<int>(std::ceil(natural)), static_cast<int>(std::lround(pixelHeight * 1.45)));
        int extra = lineHeight_ - static_cast<int>(std::ceil((ascent - descent) * scale_));
        baseline_ = extra / 2 + static_cast<int>(std::lround(ascent * scale_));
    }

protected:
    Glyph render(char32_t character) override {
        int index = stbtt_FindGlyphIndex(&file_->info, static_cast<int>(character));
        if (index == 0) {
            return fallback(character, false);
        }
        Glyph glyph;
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&file_->info, index, scale_, scale_, &x0, &y0, &x1, &y1);
        glyph.width = std::max(0, x1 - x0);
        glyph.height = std::max(0, y1 - y0);
        glyph.left = x0;
        glyph.top = baseline_ + y0;
        if (glyph.width == 0 || glyph.height == 0) {
            return glyph;
        }
        glyph.mask.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
        stbtt_MakeGlyphBitmap(&file_->info, glyph.mask.data(), glyph.width, glyph.height, glyph.width, scale_, scale_, index);
        return glyph;
    }

private:
    std::shared_ptr<FontFile> file_;
    float scale_ = 1;
};

}  // namespace

// ---------------------------------------------------------------------------

const Glyph& Font::glyph(char32_t character) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = cache_.find(character);
    if (found != cache_.end()) {
        return found->second;
    }
    return cache_.emplace(character, render(character)).first->second;
}

Glyph Font::fallback(char32_t character, bool crisp) {
    int cells = std::max(1, Font::cells(character));
    int width = cellWidth_ * cells;
    if ((character >= 0x2500 && character <= 0x257F && boxArms(character)) || (character >= 0x2580 && character <= 0x259F)) {
        return boxGlyph(character, width, lineHeight_, std::max(1, pixel_ == 1 ? lineHeight_ / 18 : pixel_));
    }
    for (const auto& file : fallbackFonts()) {
        int index = stbtt_FindGlyphIndex(&file->info, static_cast<int>(character));
        if (index == 0) {
            continue;
        }
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&file->info, &ascent, &descent, &gap);
        // Fit the font's ascent-to-descent span into the space the main font's letters use.
        double room = pixelFont_ ? pixelfont::kGlyphRows * pixel_ * 1.15 : lineHeight_ * 0.8;
        float scale = static_cast<float>(room / std::max(1, ascent - descent));
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&file->info, index, scale, scale, &x0, &y0, &x1, &y1);
        Glyph glyph;
        glyph.width = std::max(0, x1 - x0);
        glyph.height = std::max(0, y1 - y0);
        if (glyph.width == 0 || glyph.height == 0) {
            return glyph;
        }
        if (glyph.width > width) {
            // Too wide for its cells: shrink to fit.
            scale *= static_cast<float>(width) / glyph.width;
            stbtt_GetGlyphBitmapBox(&file->info, index, scale, scale, &x0, &y0, &x1, &y1);
            glyph.width = std::max(1, x1 - x0);
            glyph.height = std::max(1, y1 - y0);
        }
        glyph.mask.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
        stbtt_MakeGlyphBitmap(&file->info, glyph.mask.data(), glyph.width, glyph.height, glyph.width, scale, scale, index);
        glyph.left = (width - glyph.width) / 2;
        glyph.top = baseline_ + y0;
        if (crisp) {
            for (uint8_t& value : glyph.mask) {
                value = value >= 110 ? 255 : 0;
            }
        }
        return glyph;
    }
    // Nothing has it: a hollow box.
    Glyph glyph;
    int p = std::max(1, pixel_);
    glyph.width = std::max(3 * p, width - 2 * p);
    glyph.height = std::max(3 * p, baseline_ - 2 * p);
    glyph.left = p;
    glyph.top = 2 * p;
    glyph.mask.assign(static_cast<size_t>(glyph.width) * glyph.height, 0);
    for (int y = 0; y < glyph.height; ++y) {
        for (int x = 0; x < glyph.width; ++x) {
            if (x < p || y < p || x >= glyph.width - p || y >= glyph.height - p) {
                glyph.mask[static_cast<size_t>(y) * glyph.width + x] = 255;
            }
        }
    }
    return glyph;
}

std::shared_ptr<Font> pixelFont(int pixel) {
    static std::mutex mutex;
    static std::map<int, std::shared_ptr<Font>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    pixel = std::max(1, pixel);
    auto& font = cache[pixel];
    if (!font) {
        font = std::make_shared<PixelFace>(pixel);
    }
    return font;
}

std::shared_ptr<Font> trueTypeFont(const std::string& family, int pixelHeight) {
    static std::mutex mutex;
    static std::map<std::pair<std::string, int>, std::shared_ptr<Font>> cache;
    static std::map<std::string, std::shared_ptr<FontFile>> files;
    std::lock_guard<std::mutex> lock(mutex);
    auto key = std::make_pair(family, pixelHeight);
    if (auto found = cache.find(key); found != cache.end()) {
        return found->second;
    }
    std::shared_ptr<FontFile> file;
    if (auto loaded = files.find(family); loaded != files.end()) {
        file = loaded->second;
    } else {
        const auto& index = familyIndex();
        auto entry = index.find(family);
        if (entry == index.end()) {
            // Case-insensitive, for names typed into settings.json.
            for (const auto& [name, candidate] : index) {
                if (lower(name) == lower(family)) {
                    entry = index.find(name);
                    break;
                }
            }
        }
        if (entry != index.end()) {
            file = loadFontFile(entry->second.path);
        }
        files[family] = file;
    }
    if (!file || !file->ok) {
        return nullptr;
    }
    auto font = std::make_shared<TrueTypeFace>(file, std::max(6, pixelHeight));
    cache[key] = font;
    return font;
}

std::vector<std::string> monospaceFamilies() {
    std::vector<std::string> families;
    for (const auto& [name, entry] : familyIndex()) {
        families.push_back(name);
    }
    return families;
}

}  // namespace gw::ui
