#include "osd.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace
{
constexpr std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b)
{
    return r | (g << 8) | (b << 16) | 0xFF000000u;   // DXGI R8G8B8A8 byte order
}

constexpr std::uint32_t kInk = rgba(80, 255, 90);      // classic TV OSD green
constexpr std::uint32_t kDim = rgba(20, 70, 25);       // unlit bar segments
constexpr std::uint32_t kShadow = rgba(0, 0, 0);

// 5x7 glyphs, one byte per row (top first), bit 4 = leftmost column.
struct Glyph
{
    char c;
    std::uint8_t rows[7];
};

constexpr Glyph kFont[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } }, { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } }, { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } }, { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } }, { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } }, { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 } }, { 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } }, { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } }, { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } }, { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } }, { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } }, { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } }, { '/', { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } }, { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } }, { '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
    { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } }, { '?', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
    { '\'', { 0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00 } }, { ',', { 0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08 } },
    { '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } }, { ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
    { '&', { 0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D } }, { '#', { 0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A } },
    { '|', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } }, { '"', { 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};

const std::uint8_t* glyph(char c)
{
    for (const auto& g : kFont) {
        if (g.c == c) {
            return g.rows;
        }
    }
    return nullptr;   // space and unknown: blank
}

struct Canvas
{
    std::uint8_t* frame;
    std::uint32_t pitch;
    int w;
    int h;

    void rect(int x0, int y0, int x1, int y1, std::uint32_t color) const
    {
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(w, x1);
        y1 = std::min(h, y1);
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<std::uint32_t*>(frame + static_cast<std::size_t>(y) * pitch);
            std::fill(row + x0, row + std::max(x0, x1), color);
        }
    }

    // A triangle pointing right (dir 1) or left (dir -1) in the box [x, x+size) x [y, y+size).
    void triangle(int x, int y, int size, int dir, std::uint32_t color) const
    {
        for (int r = 0; r < size; ++r) {
            const int half = size / 2;
            const int span = (half - std::abs(r - half)) * size / std::max(1, half);   // 0 .. size .. 0
            if (dir > 0) {
                rect(x, y + r, x + span, y + r + 1, color);
            } else {
                rect(x + size - span, y + r, x + size, y + r + 1, color);
            }
        }
    }
};

// Draws `text` with shadow at (x, y), pixel size s; returns the x after it.
int text(const Canvas& c, int x, int y, int s, const std::string& t, std::uint32_t color)
{
    const int sh = std::max(1, s / 2);
    for (int pass = 0; pass < 2; ++pass) {
        int cx = x;
        const int off = pass == 0 ? sh : 0;
        const std::uint32_t col = pass == 0 ? kShadow : color;
        for (char ch : t) {
            if (const std::uint8_t* rows = glyph(ch)) {
                for (int r = 0; r < 7; ++r) {
                    for (int b = 0; b < 5; ++b) {
                        if (rows[r] & (0x10 >> b)) {
                            c.rect(cx + b * s + off, y + r * s + off, cx + (b + 1) * s + off, y + (r + 1) * s + off, col);
                        }
                    }
                }
            }
            cx += 6 * s;
        }
        if (pass == 1) {
            return cx;
        }
    }
    return x;
}

// The icon in a square of side `size` at (x, y), with shadow; returns the x after it.
int icon(const Canvas& c, int x, int y, int size, Osd::Icon kind)
{
    const int sh = std::max(1, size / 14);
    for (int pass = 0; pass < 2; ++pass) {
        const int o = pass == 0 ? sh : 0;
        const std::uint32_t col = pass == 0 ? kShadow : kInk;
        const int X = x + o, Y = y + o, S = size;
        switch (kind) {
        case Osd::Icon::Play:
            c.triangle(X, Y, S, 1, col);
            break;
        case Osd::Icon::Pause:
            c.rect(X + S / 8, Y, X + S * 3 / 8, Y + S, col);
            c.rect(X + S * 5 / 8, Y, X + S * 7 / 8, Y + S, col);
            break;
        case Osd::Icon::Stop:
            c.rect(X + S / 12, Y + S / 12, X + S * 11 / 12, Y + S * 11 / 12, col);
            break;
        case Osd::Icon::Forward:
            c.triangle(X, Y + S / 4, S / 2, 1, col);
            c.triangle(X + S / 2, Y + S / 4, S / 2, 1, col);
            break;
        case Osd::Icon::Rewind:
            c.triangle(X, Y + S / 4, S / 2, -1, col);
            c.triangle(X + S / 2, Y + S / 4, S / 2, -1, col);
            break;
        case Osd::Icon::Next:
            c.triangle(X, Y + S / 4, S / 2, 1, col);
            c.triangle(X + S * 3 / 8, Y + S / 4, S / 2, 1, col);
            c.rect(X + S * 7 / 8, Y + S / 4, X + S, Y + S * 3 / 4, col);
            break;
        case Osd::Icon::Previous:
            c.rect(X, Y + S / 4, X + S / 8, Y + S * 3 / 4, col);
            c.triangle(X + S / 8, Y + S / 4, S / 2, -1, col);
            c.triangle(X + S / 2, Y + S / 4, S / 2, -1, col);
            break;
        case Osd::Icon::Volume:   // a speaker: box + cone opening to the right
            c.rect(X, Y + S * 3 / 10, X + S * 3 / 10, Y + S * 7 / 10, col);
            for (int r = 0; r < S; ++r) {   // cone: the box's height at the box, full height at the right edge
                const int d = std::abs(r - S / 2);                       // 0 .. S/2 from the centre row
                const int x0 = X + S * 3 / 10 + std::max(0, d - S / 5) * 5 / 3;
                c.rect(std::min(x0, X + S * 8 / 10), Y + r, X + S * 8 / 10, Y + r + 1, col);
            }
            break;
        case Osd::Icon::None:
            break;
        }
    }
    return kind == Osd::Icon::None ? x : x + size + size / 3;
}
}

void drawOsd(std::uint8_t* frame, std::uint32_t pitch, std::uint32_t width, std::uint32_t height, const Osd& osd)
{
    const Canvas c{ frame, pitch, static_cast<int>(width), static_cast<int>(height) };
    const int s = std::max(2, static_cast<int>(height) / 110);   // font pixel: ~6 px on a 724-high screen
    const int margin = static_cast<int>(width) / 24;
    int x = margin;
    const int y = static_cast<int>(height) / 14;
    x = icon(c, x, y, 7 * s, osd.icon);
    x = text(c, x, y, s, osd.label, kInk);
    if (osd.level >= 0.0f) {   // segment bar after the label
        const int segments = 20;
        const int segW = 2 * s;
        int bx = x + 2 * s;
        const int lit = static_cast<int>(std::lround(std::clamp(osd.level, 0.0f, 1.0f) * segments));
        for (int i = 0; i < segments; ++i) {
            c.rect(bx + s / 2, y + s / 2 + s / 2, bx + segW + s / 2, y + 7 * s + s / 2, kShadow);
            c.rect(bx, y + s / 2, bx + segW, y + 7 * s, i < lit ? kInk : kDim);
            bx += segW + s;
        }
    }
    if (!osd.detail.empty()) {
        const int s2 = std::max(2, s * 3 / 5);
        text(c, margin, y + 9 * s, s2, osd.detail, kInk);
    }
}

std::string osdText(const std::string& utf8, std::size_t maxChars)
{
    std::string out;
    for (std::size_t i = 0; i < utf8.size() && out.size() < maxChars;) {
        const unsigned char b = static_cast<unsigned char>(utf8[i]);
        if (b < 0x80) {
            const char ch = static_cast<char>(std::toupper(b));
            out.push_back(glyph(ch) || ch == ' ' ? ch : '?');
            ++i;
            continue;
        }
        // two-byte Latin-1 range: accents to the base letter
        if ((b & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            const unsigned cp = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            char base = '?';
            if ((cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xE0 && cp <= 0xE5)) {
                base = 'A';
            } else if (cp == 0xC7 || cp == 0xE7) {
                base = 'C';
            } else if ((cp >= 0xC8 && cp <= 0xCB) || (cp >= 0xE8 && cp <= 0xEB)) {
                base = 'E';
            } else if ((cp >= 0xCC && cp <= 0xCF) || (cp >= 0xEC && cp <= 0xEF)) {
                base = 'I';
            } else if (cp == 0xD1 || cp == 0xF1) {
                base = 'N';
            } else if ((cp >= 0xD2 && cp <= 0xD6) || (cp >= 0xF2 && cp <= 0xF6)) {
                base = 'O';
            } else if ((cp >= 0xD9 && cp <= 0xDC) || (cp >= 0xF9 && cp <= 0xFC)) {
                base = 'U';
            } else if (cp == 0xBF || cp == 0xA1) {   // inverted ? and ! (Spanish)
                i += 2;
                continue;
            }
            out.push_back(base);
            i += 2;
            continue;
        }
        // longer sequences (CJK, emoji, ...): one '?', skip the continuation bytes
        out.push_back('?');
        ++i;
        while (i < utf8.size() && (static_cast<unsigned char>(utf8[i]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    if (out.size() >= maxChars && maxChars > 3) {   // cut: mark it
        out.resize(maxChars - 3);
        out += "...";
    }
    return out;
}
