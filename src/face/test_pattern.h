#pragma once
// ── test_pattern.h ────────────────────────────────────────────────────────────
// Diagnostic overlays for setting up a multi-panel HUB75 face: which physical
// panel is which link in the chain, whether the per-panel nudges line the set up
// the way it's actually mounted, and whether each panel's flip / colour order is
// right. Drawn over the finished canvas in canvas space, so everything the
// renderer does downstream — the per-panel mounting flips, the gather into the
// chain-order framebuffer, the whole-output mirror — applies to the pattern too.
// That's deliberate: the pattern is only useful if it goes through the exact
// path the face does, so what you see on the panels is the real mapping and not
// a second, parallel guess at it.
//
// Reading them:
//   PanelIds   the panel showing "1" is Panel 1 in the layout editor and the
//              first link in the chain. A digit that reads backwards or upside
//              down means that panel's Flip Horizontal / Vertical is wrong; a
//              digit on the wrong physical panel means the chain order (or the
//              Serpentine Chain toggle) is.
//   Grid       one continuous 8px grid over the whole canvas. Lines that step
//              at a panel seam mean that panel's Offset X/Y doesn't match where
//              it physically sits.
//   Edges      every panel's outline plus corner pips and a centre cross —
//              a missing edge or clipped pip means pixels are falling off.
//   ColorBars  red / green / blue / white / black bars per panel. Bars in the
//              wrong order mean that build needs a different Color Order.
//   Sweep      a line sweeping across the whole set, horizontal then vertical.
//              It should cross the seams without jumping or stalling.
//   Chase      lights one panel at a time in chain order — the clearest read of
//              how the ribbon actually runs.
//
// Threading: same contract as GlitchEffect / ScrollText — one instance owned and
// driven by the NativeFaceController render thread (tick + render); the pattern
// is set from other threads through set_pattern(), which is atomic.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "face_config.h"
#include "max_section_content.h"

namespace face {

enum class TestPattern : uint8_t {
    Off       = 0,
    PanelIds  = 1,
    Grid      = 2,
    Edges     = 3,
    ColorBars = 4,
    Sweep     = 5,
    Chase     = 6,
};

inline const char* test_pattern_name(TestPattern p) {
    switch (p) {
        case TestPattern::PanelIds:  return "Panel IDs";
        case TestPattern::Grid:      return "Alignment Grid";
        case TestPattern::Edges:     return "Panel Edges";
        case TestPattern::ColorBars: return "Color Bars";
        case TestPattern::Sweep:     return "Sweep";
        case TestPattern::Chase:     return "Chain Chase";
        default:                     return "Off";
    }
}

class TestPatternRenderer {
public:
    void set_pattern(TestPattern p) { pattern_.store(static_cast<uint8_t>(p)); }
    TestPattern pattern() const {
        return static_cast<TestPattern>(pattern_.load());
    }
    bool active() const { return pattern() != TestPattern::Off; }

    // Render-thread only.
    void tick(double dt) { t_ += dt; }

    // Paints over `canvas` (CV_8UC3). `panels` are the physical panels in layout
    // order — index i is also link i of the chain — with rects in canvas space.
    // With no panel list (daemon mode) the whole-canvas patterns still work and
    // the per-panel ones treat the canvas as one panel.
    void render(cv::Mat& canvas,
                const std::vector<RenderConfig::OutputPanel>& panels) {
        const TestPattern p = pattern();
        if (p == TestPattern::Off || canvas.empty()) return;

        std::vector<cv::Rect> rects;
        rects.reserve(panels.size());
        for (const auto& op : panels) {
            const cv::Rect r = cv::Rect(op.x, op.y, op.w, op.h) &
                               cv::Rect(0, 0, canvas.cols, canvas.rows);
            if (r.width > 0 && r.height > 0) rects.push_back(r);
        }
        if (rects.empty()) rects.push_back(cv::Rect(0, 0, canvas.cols, canvas.rows));

        switch (p) {
            case TestPattern::PanelIds:  draw_panel_ids(canvas, rects);  break;
            case TestPattern::Grid:      draw_grid(canvas, rects);       break;
            case TestPattern::Edges:     draw_edges(canvas, rects);      break;
            case TestPattern::ColorBars: draw_color_bars(canvas, rects); break;
            case TestPattern::Sweep:     draw_sweep(canvas, rects);      break;
            case TestPattern::Chase:     draw_chase(canvas, rects);      break;
            default: break;
        }
    }

private:
    // Per-panel accent, so neighbouring panels never share a colour.
    static cv::Scalar accent(size_t i) {
        static const cv::Scalar kAccents[] = {
            {255,  64,  64},   // red
            { 64, 255,  96},   // green
            { 96, 160, 255},   // blue
            {255, 208,  64},   // amber
        };
        return kAccents[i % 4];
    }

    // Blit `text` centred in `r` at an integer upscale, tinted. Uses the same
    // 5×7 font as the scroll banner so glyph metrics match the rest of the HUD.
    static void draw_label(cv::Mat& canvas, const cv::Rect& r,
                           const std::string& text, int scale,
                           const cv::Scalar& color) {
        const int tw = max_content::text_width(text);
        if (tw <= 0 || scale <= 0) return;
        cv::Mat mono(7, tw, CV_8UC3, cv::Scalar(0, 0, 0));
        max_content::draw_text(mono, text, 0, 0);
        // Shrink the upscale until the glyphs fit the panel.
        while (scale > 1 && (tw * scale > r.width || 7 * scale > r.height)) --scale;
        cv::Mat big;
        cv::resize(mono, big, cv::Size(tw * scale, 7 * scale), 0, 0, cv::INTER_NEAREST);
        cv::Mat mask;
        cv::cvtColor(big, mask, cv::COLOR_RGB2GRAY);

        const int x = r.x + (r.width  - big.cols) / 2;
        const int y = r.y + (r.height - big.rows) / 2;
        const cv::Rect dst = cv::Rect(x, y, big.cols, big.rows) &
                             cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (dst.width <= 0 || dst.height <= 0) return;
        const cv::Rect src(dst.x - x, dst.y - y, dst.width, dst.height);
        cv::Mat tinted(dst.size(), CV_8UC3, color);
        tinted.copyTo(canvas(dst), mask(src));
    }

    static void outline(cv::Mat& canvas, const cv::Rect& r, const cv::Scalar& c) {
        cv::rectangle(canvas, cv::Rect(r.x, r.y, r.width, r.height),
                      c, 1, cv::LINE_8);
    }

    // A pip in the top-left corner: tells you a panel's orientation even when
    // the glyph itself is roughly symmetric.
    static void corner_pip(cv::Mat& canvas, const cv::Rect& r,
                           const cv::Scalar& c) {
        const int s = std::max(2, std::min(r.width, r.height) / 10);
        const cv::Rect pip = cv::Rect(r.x + 1, r.y + 1, s, s) &
                             cv::Rect(0, 0, canvas.cols, canvas.rows);
        if (pip.width > 0 && pip.height > 0) canvas(pip).setTo(c);
    }

    void draw_panel_ids(cv::Mat& canvas,
                        const std::vector<cv::Rect>& rects) const {
        canvas.setTo(cv::Scalar(0, 0, 0));
        for (size_t i = 0; i < rects.size(); ++i) {
            const cv::Scalar c = accent(i);
            // Dim wash so the panel's extent is visible even where the glyph
            // isn't, without washing out the number itself.
            canvas(rects[i]).setTo(c * 0.12);
            outline(canvas, rects[i], c);
            corner_pip(canvas, rects[i], cv::Scalar(255, 255, 255));
            draw_label(canvas, rects[i], std::to_string(i + 1), 3, c);
        }
    }

    void draw_grid(cv::Mat& canvas,
                   const std::vector<cv::Rect>& rects) const {
        canvas.setTo(cv::Scalar(0, 0, 0));
        // One continuous grid over the whole canvas — the point is that it does
        // NOT restart per panel, so a panel sitting where the layout doesn't
        // think it does shows up as a step in the lines.
        const int step = 8;
        const cv::Scalar minor(0, 90, 90), major(0, 190, 190);
        for (int x = 0; x < canvas.cols; x += step) {
            const cv::Scalar c = (x % (step * 4) == 0) ? major : minor;
            canvas(cv::Rect(x, 0, 1, canvas.rows)).setTo(c);
        }
        for (int y = 0; y < canvas.rows; y += step) {
            const cv::Scalar c = (y % (step * 4) == 0) ? major : minor;
            canvas(cv::Rect(0, y, canvas.cols, 1)).setTo(c);
        }
        for (size_t i = 0; i < rects.size(); ++i)
            outline(canvas, rects[i], accent(i));
    }

    void draw_edges(cv::Mat& canvas,
                    const std::vector<cv::Rect>& rects) const {
        canvas.setTo(cv::Scalar(0, 0, 0));
        for (size_t i = 0; i < rects.size(); ++i) {
            const cv::Rect& r = rects[i];
            const cv::Scalar c = accent(i);
            outline(canvas, r, c);
            // Corner pips: any that's clipped means pixels are falling off the
            // panel — the usual sign of an off-by-one nudge or a wrong size.
            const int s = std::max(2, std::min(r.width, r.height) / 8);
            const cv::Scalar w(255, 255, 255);
            canvas(cv::Rect(r.x, r.y, s, 1)).setTo(w);
            canvas(cv::Rect(r.x, r.y, 1, s)).setTo(w);
            canvas(cv::Rect(r.x + r.width - s, r.y, s, 1)).setTo(w);
            canvas(cv::Rect(r.x + r.width - 1, r.y, 1, s)).setTo(w);
            canvas(cv::Rect(r.x, r.y + r.height - 1, s, 1)).setTo(w);
            canvas(cv::Rect(r.x, r.y + r.height - s, 1, s)).setTo(w);
            canvas(cv::Rect(r.x + r.width - s, r.y + r.height - 1, s, 1)).setTo(w);
            canvas(cv::Rect(r.x + r.width - 1, r.y + r.height - s, 1, s)).setTo(w);
            // Centre cross.
            const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
            const int arm = std::max(2, std::min(r.width, r.height) / 6);
            const cv::Rect h = cv::Rect(cx - arm, cy, arm * 2, 1) &
                               cv::Rect(0, 0, canvas.cols, canvas.rows);
            const cv::Rect v = cv::Rect(cx, cy - arm, 1, arm * 2) &
                               cv::Rect(0, 0, canvas.cols, canvas.rows);
            if (h.width > 0 && h.height > 0) canvas(h).setTo(c);
            if (v.width > 0 && v.height > 0) canvas(v).setTo(c);
        }
    }

    void draw_color_bars(cv::Mat& canvas,
                         const std::vector<cv::Rect>& rects) const {
        canvas.setTo(cv::Scalar(0, 0, 0));
        // Canvas order is RGB (same as draw_text's), so these are literal.
        static const cv::Scalar kBars[] = {
            {255,   0,   0}, {  0, 255,   0}, {  0,   0, 255},
            {255, 255, 255}, {128, 128, 128},
        };
        constexpr int kN = 5;
        for (const cv::Rect& r : rects) {
            for (int b = 0; b < kN; ++b) {
                const int x0 = r.x + (r.width * b)       / kN;
                const int x1 = r.x + (r.width * (b + 1)) / kN;
                if (x1 <= x0) continue;
                canvas(cv::Rect(x0, r.y, x1 - x0, r.height)).setTo(kBars[b]);
            }
        }
    }

    void draw_sweep(cv::Mat& canvas,
                    const std::vector<cv::Rect>& rects) const {
        canvas.setTo(cv::Scalar(0, 0, 0));
        for (size_t i = 0; i < rects.size(); ++i)
            outline(canvas, rects[i], accent(i) * 0.35);
        // One horizontal pass then one vertical, ~2s each, so a seam that
        // swallows or repeats a column shows up as a stutter in the travel.
        constexpr double kPass = 2.0;
        const double phase = std::fmod(t_, kPass * 2.0);
        const cv::Scalar lead(255, 255, 255), trail(0, 200, 255);
        if (phase < kPass) {
            const int x = static_cast<int>((phase / kPass) * canvas.cols);
            for (int k = 0; k < 4; ++k) {
                const int xx = x - k;
                if (xx < 0 || xx >= canvas.cols) continue;
                canvas(cv::Rect(xx, 0, 1, canvas.rows))
                    .setTo(k == 0 ? lead : trail * (1.0 - k * 0.25));
            }
        } else {
            const int y = static_cast<int>(((phase - kPass) / kPass) * canvas.rows);
            for (int k = 0; k < 4; ++k) {
                const int yy = y - k;
                if (yy < 0 || yy >= canvas.rows) continue;
                canvas(cv::Rect(0, yy, canvas.cols, 1))
                    .setTo(k == 0 ? lead : trail * (1.0 - k * 0.25));
            }
        }
    }

    void draw_chase(cv::Mat& canvas,
                    const std::vector<cv::Rect>& rects) const {
        canvas.setTo(cv::Scalar(0, 0, 0));
        constexpr double kHold = 1.0;   // seconds per panel
        const size_t n = rects.size();
        const size_t lit = static_cast<size_t>(
            std::fmod(t_ / kHold, static_cast<double>(n)));
        for (size_t i = 0; i < n; ++i) {
            if (i != lit) { outline(canvas, rects[i], accent(i) * 0.3); continue; }
            const cv::Scalar c = accent(i);
            canvas(rects[i]).setTo(c * 0.35);
            outline(canvas, rects[i], c);
            draw_label(canvas, rects[i], std::to_string(i + 1), 3,
                       cv::Scalar(255, 255, 255));
        }
    }

    std::atomic<uint8_t> pattern_{0};
    double               t_ = 0.0;
};

}  // namespace face
