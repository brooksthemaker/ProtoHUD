#pragma once
// ── tiny_font.h ───────────────────────────────────────────────────────────────
// A 3×5 pixel font for the face banner, alongside the 5×7 one in
// max_section_content. At 1× it advances 4px per character against the 5×7's 6
// and stands 5 rows tall against 7 — so a 32px panel fits five stacked lines
// where the larger font manages three, which is what makes a full diagnostics
// readout land on a single panel.
//
// Legibility is the trade: at three pixels wide some letters are necessarily
// close (N/D, M/W differ by a row), so this is for dense readouts rather than
// for a message that has to be read at a glance across a room.
//
// Glyphs are written as row strings so they can be read and corrected in place;
// they're packed into column bitmasks (matching max_content's Glyph layout) once
// on first use. Same character set as the 5×7 font — 0-9, A-Z, space and
// ! ? . : - + / = < > — so switching fonts never silently drops a character.

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>

#include <opencv2/core.hpp>

namespace face {
namespace tiny_font {

inline constexpr int kW       = 3;   // glyph width
inline constexpr int kH       = 5;   // glyph height
inline constexpr int kAdvance = 4;   // width + 1px gap

using Glyph = std::array<uint8_t, kW>;   // one byte per column, bit r = row r

inline const std::unordered_map<char, Glyph>& font() {
    struct Row { char ch; const char* r[kH]; };
    static const Row kRows[] = {
        {' ', {"...","...","...","...","..."}},
        {'0', {"###","#.#","#.#","#.#","###"}},
        {'1', {".#.","##.",".#.",".#.","###"}},
        {'2', {"###","..#","###","#..","###"}},
        {'3', {"###","..#","###","..#","###"}},
        {'4', {"#.#","#.#","###","..#","..#"}},
        {'5', {"###","#..","###","..#","###"}},
        {'6', {"###","#..","###","#.#","###"}},
        {'7', {"###","..#","..#","..#","..#"}},
        {'8', {"###","#.#","###","#.#","###"}},
        {'9', {"###","#.#","###","..#","###"}},
        {'A', {"###","#.#","###","#.#","#.#"}},
        {'B', {"##.","#.#","##.","#.#","##."}},
        {'C', {"###","#..","#..","#..","###"}},
        {'D', {"##.","#.#","#.#","#.#","##."}},
        {'E', {"###","#..","##.","#..","###"}},
        {'F', {"###","#..","##.","#..","#.."}},
        {'G', {"###","#..","#.#","#.#","###"}},
        {'H', {"#.#","#.#","###","#.#","#.#"}},
        {'I', {"###",".#.",".#.",".#.","###"}},
        {'J', {"..#","..#","..#","#.#","###"}},
        {'K', {"#.#","#.#","##.","#.#","#.#"}},
        {'L', {"#..","#..","#..","#..","###"}},
        {'M', {"#.#","###","###","#.#","#.#"}},
        {'N', {"##.","#.#","#.#","#.#","#.#"}},
        {'O', {"###","#.#","#.#","#.#","###"}},
        {'P', {"###","#.#","###","#..","#.."}},
        {'Q', {"###","#.#","#.#","###","..#"}},
        {'R', {"###","#.#","##.","#.#","#.#"}},
        {'S', {"###","#..","###","..#","###"}},
        {'T', {"###",".#.",".#.",".#.",".#."}},
        {'U', {"#.#","#.#","#.#","#.#","###"}},
        {'V', {"#.#","#.#","#.#","#.#",".#."}},
        {'W', {"#.#","#.#","###","###","#.#"}},
        {'X', {"#.#","#.#",".#.","#.#","#.#"}},
        {'Y', {"#.#","#.#","###",".#.",".#."}},
        {'Z', {"###","..#",".#.","#..","###"}},
        {'!', {".#.",".#.",".#.","...",".#."}},
        {'?', {"###","..#",".##","...",".#."}},
        {'.', {"...","...","...","...",".#."}},
        {':', {"...",".#.","...",".#.","..."}},
        {'-', {"...","...","###","...","..."}},
        {'+', {"...",".#.","###",".#.","..."}},
        {'/', {"..#","..#",".#.","#..","#.."}},
        {'=', {"...","###","...","###","..."}},
        {'<', {"..#",".#.","#..",".#.","..#"}},
        {'>', {"#..",".#.","..#",".#.","#.."}},
    };
    static const std::unordered_map<char, Glyph> kFont = [] {
        std::unordered_map<char, Glyph> m;
        for (const Row& row : kRows) {
            Glyph g{};
            for (int r = 0; r < kH; ++r)
                for (int c = 0; c < kW; ++c)
                    if (row.r[r][c] == '#') g[c] |= static_cast<uint8_t>(1u << r);
            m.emplace(row.ch, g);
        }
        return m;
    }();
    return kFont;
}

// Paint `text` white into `canvas` (CV_8UC3) with its top-left at (x, y);
// returns the x just past the last glyph. Unknown characters advance blank,
// matching the 5×7 font's behaviour.
inline int draw_text(cv::Mat& canvas, const std::string& text, int x, int y) {
    for (char ch : text) {
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const auto it = font().find(up);
        if (it != font().end()) {
            const Glyph& g = it->second;
            for (int c = 0; c < kW; ++c)
                for (int r = 0; r < kH; ++r) {
                    if (!(g[c] & (1u << r))) continue;
                    const int px = x + c, py = y + r;
                    if (px >= 0 && py >= 0 && px < canvas.cols && py < canvas.rows)
                        canvas.at<cv::Vec3b>(py, px) = cv::Vec3b(255, 255, 255);
                }
        }
        x += kAdvance;
    }
    return x;
}

inline int text_width(const std::string& text) {
    return static_cast<int>(text.size()) * kAdvance;
}

}  // namespace tiny_font
}  // namespace face
