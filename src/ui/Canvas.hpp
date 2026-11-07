#pragma once

#include "Theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gw::ui {

/// 0xRRGGBBAA, as everywhere in the core.
using gw::Color;

/// A rectangle in device pixels, top left origin.
struct Rect {
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(int dx, int dy) const { return {x + dx, y + dy, std::max(0, w - 2 * dx), std::max(0, h - 2 * dy)}; }
    Rect offset(int dx, int dy) const { return {x + dx, y + dy, w, h}; }
    Rect intersect(const Rect& other) const {
        int left = std::max(x, other.x), top = std::max(y, other.y);
        int r = std::min(right(), other.right()), b = std::min(bottom(), other.bottom());
        return {left, top, std::max(0, r - left), std::max(0, b - top)};
    }
    Rect unite(const Rect& other) const {
        if (empty()) {
            return other;
        }
        if (other.empty()) {
            return *this;
        }
        int left = std::min(x, other.x), top = std::min(y, other.y);
        return {left, top, std::max(right(), other.right()) - left, std::max(bottom(), other.bottom()) - top};
    }
    bool operator==(const Rect&) const = default;

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Point {
    double x = 0;
    double y = 0;
};

// ---- Colors
constexpr Color kBlackColor = 0x000000FF;
constexpr Color kWhiteColor = 0xFFFFFFFF;
constexpr Color kClear = 0x00000000;

/// `color` with its alpha multiplied by `opacity` (0..1).
Color withOpacity(Color color, double opacity);
/// Straight mix of two colors (0: a, 1: b), alpha included.
Color mix(Color a, Color b, double amount);

/// A frame buffer the window shows: 32-bit pixels, 0xAARRGGBB in memory order as SDL's ARGB8888.
/// Every drawing call respects the current clip.
class Canvas {
public:
    // ---- Size
    void resize(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }
    const uint32_t* data() const { return pixels_.data(); }
    Rect bounds() const { return {0, 0, width_, height_}; }

    // ---- Clipping
    void pushClip(const Rect& rect);
    void popClip();
    const Rect& clip() const { return clip_; }

    // ---- Filling
    void fill(const Rect& rect, Color color);
    /// A checkerboard of two colors in squares of `cell` pixels, anchored to the canvas, like the
    /// shaded characters of a text screen.
    void fillChecker(const Rect& rect, Color first, Color second, int cell);
    /// A line of `thickness` pixels just inside the rectangle's edge.
    void frame(const Rect& rect, Color color, int thickness);

    // ---- Masks and images
    /// Draws an 8-bit coverage mask in `color`, top left at (x, y).
    void blendMask(const uint8_t* mask, int maskWidth, int maskHeight, int x, int y, Color color, double opacity = 1);
    /// Copies RGBA pixels (row after row, 4 bytes each), scaled by a whole number, blending by their alpha.
    void drawImage(const uint8_t* rgba, int imageWidth, int imageHeight, int x, int y, int scale);

    // ---- Shapes
    /// A convex polygon, each pixel taking `colorAt` of its center (for gradients). Inside test is
    /// by pixel centers, so shapes keep hard edges.
    template <typename ColorAt>
    void fillConvex(const std::vector<Point>& points, ColorAt&& colorAt);

private:
    void blendPixel(uint32_t& target, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha);

    int width_ = 0;
    int height_ = 0;
    std::vector<uint32_t> pixels_;
    Rect clip_;
    std::vector<Rect> clips_;
};

// ---------------------------------------------------------------------------

inline void Canvas::blendPixel(uint32_t& target, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha) {
    if (alpha >= 255) {
        target = 0xFF000000u | (red << 16) | (green << 8) | blue;
        return;
    }
    if (alpha == 0) {
        return;
    }
    uint32_t inverse = 255 - alpha;
    uint32_t oldRed = (target >> 16) & 0xFF, oldGreen = (target >> 8) & 0xFF, oldBlue = target & 0xFF;
    uint32_t newRed = (red * alpha + oldRed * inverse + 127) / 255;
    uint32_t newGreen = (green * alpha + oldGreen * inverse + 127) / 255;
    uint32_t newBlue = (blue * alpha + oldBlue * inverse + 127) / 255;
    target = 0xFF000000u | (newRed << 16) | (newGreen << 8) | newBlue;
}

template <typename ColorAt>
void Canvas::fillConvex(const std::vector<Point>& points, ColorAt&& colorAt) {
    if (points.size() < 3) {
        return;
    }
    double top = points[0].y, bottom = points[0].y;
    for (const Point& point : points) {
        top = std::min(top, point.y);
        bottom = std::max(bottom, point.y);
    }
    int firstRow = std::max(clip_.y, static_cast<int>(std::floor(top)));
    int lastRow = std::min(clip_.bottom() - 1, static_cast<int>(std::ceil(bottom)));
    for (int row = firstRow; row <= lastRow; ++row) {
        double centerY = row + 0.5;
        double left = 1e18, right = -1e18;
        for (size_t index = 0; index < points.size(); ++index) {
            const Point& a = points[index];
            const Point& b = points[(index + 1) % points.size()];
            if ((a.y <= centerY && b.y > centerY) || (b.y <= centerY && a.y > centerY)) {
                double t = (centerY - a.y) / (b.y - a.y);
                double crossing = a.x + t * (b.x - a.x);
                left = std::min(left, crossing);
                right = std::max(right, crossing);
            }
        }
        if (left > right) {
            continue;
        }
        int firstColumn = std::max(clip_.x, static_cast<int>(std::ceil(left - 0.5)));
        int lastColumn = std::min(clip_.right() - 1, static_cast<int>(std::floor(right - 0.5)));
        uint32_t* line = pixels_.data() + static_cast<size_t>(row) * width_;
        for (int column = firstColumn; column <= lastColumn; ++column) {
            Color color = colorAt(column + 0.5, centerY);
            blendPixel(line[column], (color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
        }
    }
}

}  // namespace gw::ui
