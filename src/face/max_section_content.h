#pragma once
// ── max_section_content.h ────────────────────────────────────────────────────
// Content renderer for MAX7219 "section" panels — the extra surface that runs
// beside the HUB75 face (see docs/coprocessor-input.md). Renders a named symbol,
// a short text string, or a generated pattern into an RGB canvas that
// Max7219PanelOutput thresholds to mono. Pure drawing — no I/O, no state — so it
// unit-tests cleanly and MaxSectionController just owns the canvas + which
// content is current.
//
// Coordinates are canvas pixels; "on" pixels are painted white. Symbols are 8×8
// (one MAX7219 module); text uses a built-in 5×7 font, left→right with a 1px gap.

#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace face {

namespace max_content {

// Built-in symbol / pattern names, in the order Next/Prev cycles them. Symbols
// are single 8×8 glyphs (tiled across a wider grid); patterns fill the grid.
const std::vector<std::string>& symbol_names();
const std::vector<std::string>& pattern_names();

// Paint a symbol (tiled to cover the whole canvas), a pattern, or text into
// `canvas` (CV_8UC3, pre-cleared by the caller). Unknown names paint nothing.
// `phase` animates patterns (e.g. a scrolling bar); pass a frame counter.
void draw_symbol (cv::Mat& canvas, const std::string& name);
void draw_pattern(cv::Mat& canvas, const std::string& name, int phase);
// Render text starting at (x, y); returns the x just past the last glyph so the
// caller can measure / right-justify. Glyphs the font lacks render as blank.
int  draw_text   (cv::Mat& canvas, const std::string& text, int x, int y);
int  text_width  (const std::string& text);   // pixels, same metrics as draw_text

}  // namespace max_content
}  // namespace face
