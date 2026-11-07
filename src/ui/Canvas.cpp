#include "Canvas.hpp"

#include <cmath>
#include <cstring>

namespace gw::ui {

Color withOpacity(Color color, double opacity) {
    double alpha = (color & 0xFF) * std::clamp(opacity, 0.0, 1.0);
    return (color & 0xFFFFFF00u) | static_cast<uint32_t>(std::lround(alpha));
}

Color mix(Color a, Color b, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    auto channel = [&](int shift) {
        double first = (a >> shift) & 0xFF, second = (b >> shift) & 0xFF;
        return static_cast<uint32_t>(std::lround(first + (second - first) * amount)) << shift;
    };
    return channel(24) | channel(16) | channel(8) | channel(0);
}

void Canvas::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    pixels_.assign(static_cast<size_t>(width_) * height_, 0xFF000000u);
    clips_.clear();
    clip_ = bounds();
}

void Canvas::pushClip(const Rect& rect) {
    clips_.push_back(clip_);
    clip_ = clip_.intersect(rect);
}

void Canvas::popClip() {
    if (!clips_.empty()) {
        clip_ = clips_.back();
        clips_.pop_back();
    } else {
        clip_ = bounds();
    }
}

void Canvas::fill(const Rect& rect, Color color) {
    Rect area = rect.intersect(clip_);
    uint32_t alpha = color & 0xFF;
    if (area.empty() || alpha == 0) {
        return;
    }
    uint32_t red = (color >> 24) & 0xFF, green = (color >> 16) & 0xFF, blue = (color >> 8) & 0xFF;
    if (alpha == 255) {
        uint32_t value = 0xFF000000u | (red << 16) | (green << 8) | blue;
        for (int row = area.y; row < area.bottom(); ++row) {
            uint32_t* line = pixels_.data() + static_cast<size_t>(row) * width_ + area.x;
            std::fill(line, line + area.w, value);
        }
        return;
    }
    for (int row = area.y; row < area.bottom(); ++row) {
        uint32_t* line = pixels_.data() + static_cast<size_t>(row) * width_;
        for (int column = area.x; column < area.right(); ++column) {
            blendPixel(line[column], red, green, blue, alpha);
        }
    }
}

void Canvas::fillChecker(const Rect& rect, Color first, Color second, int cell) {
    Rect area = rect.intersect(clip_);
    if (area.empty()) {
        return;
    }
    cell = std::max(1, cell);
    auto packed = [](Color color) {
        return 0xFF000000u | (((color >> 24) & 0xFF) << 16) | (((color >> 16) & 0xFF) << 8) | ((color >> 8) & 0xFF);
    };
    uint32_t a = packed(first), b = packed(second);
    for (int row = area.y; row < area.bottom(); ++row) {
        uint32_t* line = pixels_.data() + static_cast<size_t>(row) * width_;
        int rowParity = (row / cell) & 1;
        for (int column = area.x; column < area.right(); ++column) {
            line[column] = (((column / cell) & 1) ^ rowParity) ? b : a;
        }
    }
}

void Canvas::frame(const Rect& rect, Color color, int thickness) {
    fill({rect.x, rect.y, rect.w, thickness}, color);
    fill({rect.x, rect.bottom() - thickness, rect.w, thickness}, color);
    fill({rect.x, rect.y + thickness, thickness, rect.h - 2 * thickness}, color);
    fill({rect.right() - thickness, rect.y + thickness, thickness, rect.h - 2 * thickness}, color);
}

void Canvas::blendMask(const uint8_t* mask, int maskWidth, int maskHeight, int x, int y, Color color, double opacity) {
    Rect area = Rect{x, y, maskWidth, maskHeight}.intersect(clip_);
    if (area.empty()) {
        return;
    }
    uint32_t red = (color >> 24) & 0xFF, green = (color >> 16) & 0xFF, blue = (color >> 8) & 0xFF;
    double baseAlpha = (color & 0xFF) * std::clamp(opacity, 0.0, 1.0) / 255.0;
    for (int row = area.y; row < area.bottom(); ++row) {
        const uint8_t* source = mask + static_cast<size_t>(row - y) * maskWidth + (area.x - x);
        uint32_t* line = pixels_.data() + static_cast<size_t>(row) * width_;
        for (int column = area.x; column < area.right(); ++column, ++source) {
            if (*source == 0) {
                continue;
            }
            uint32_t alpha = static_cast<uint32_t>(*source * baseAlpha + 0.5);
            blendPixel(line[column], red, green, blue, alpha);
        }
    }
}

void Canvas::drawImage(const uint8_t* rgba, int imageWidth, int imageHeight, int x, int y, int scale) {
    scale = std::max(1, scale);
    Rect area = Rect{x, y, imageWidth * scale, imageHeight * scale}.intersect(clip_);
    for (int row = area.y; row < area.bottom(); ++row) {
        int sourceRow = (row - y) / scale;
        uint32_t* line = pixels_.data() + static_cast<size_t>(row) * width_;
        for (int column = area.x; column < area.right(); ++column) {
            int sourceColumn = (column - x) / scale;
            const uint8_t* pixel = rgba + (static_cast<size_t>(sourceRow) * imageWidth + sourceColumn) * 4;
            blendPixel(line[column], pixel[0], pixel[1], pixel[2], pixel[3]);
        }
    }
}

}  // namespace gw::ui
