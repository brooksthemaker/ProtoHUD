// Headless check for face/edge_fade.h — proves the fade follows the OUTER RIM
// of the panel set and not each panel's own edges.
//
// The distinction is the whole point: on the VANESSA2 build the bottom two
// panels share a full 32px edge and each top panel shares ~50px with the one
// below it, so a naive per-panel vignette would paint dark bands straight
// through a continuous image. These assertions pin that down.
//
// build: g++ -std=c++17 -O2 -I src scripts/face_diag/edge_fade_check.cpp \
//            $(pkg-config --cflags --libs opencv4) -o /tmp/edge_fade_check
#include "face/edge_fade.h"
#include <cstdio>
#include <vector>

static int fails = 0;
static void chk(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "PASS" : "*** FAIL ***");
    if (!ok) ++fails;
}

int main() {
    // Real VANESSA2 geometry, as computed from the live config.
    const std::vector<cv::Rect> panels = {
        {  1,  3, 64, 32 },   // P0 top-left   (rot +6)
        { 95,  3, 64, 32 },   // P1 top-right  (rot -6)
        { 15, 35, 64, 32 },   // P2 bottom-left
        { 79, 35, 64, 32 },   // P3 bottom-right
    };
    const cv::Size canvas_sz(160, 70);
    const int W = 8;

    auto run = [&](bool dither) {
        cv::Mat c(canvas_sz, CV_8UC3, cv::Scalar(255, 255, 255));
        face::EdgeFade ef;
        ef.apply(c, panels, W, dither);
        return c;
    };
    auto lum = [](const cv::Mat& m, int x, int y) {
        return static_cast<int>(m.at<cv::Vec3b>(y, x)[0]);
    };

    std::printf("edge_fade_check — VANESSA2 geometry, ramp %dpx\n\n", W);

    cv::Mat m = run(/*dither=*/false);

    std::printf("interior seams must NOT fade (this is the design claim):\n");
    // P2|P3 share the edge at x=79 over the full 32px height.
    chk(lum(m, 79, 51) == 255, "P2|P3 vertical seam at x=79 is full brightness");
    chk(lum(m, 78, 51) == 255 && lum(m, 80, 51) == 255,
        "  ...and both pixels either side of it");
    // P0 bottom (y=35) meets P2 top over x 15..65.
    chk(lum(m, 40, 35) == 255, "P0/P2 horizontal seam at y=35 is full brightness");
    chk(lum(m, 40, 34) == 255 && lum(m, 40, 36) == 255,
        "  ...and both pixels either side of it");
    chk(lum(m, 120, 35) == 255, "P1/P3 horizontal seam at y=35 is full brightness");

    std::printf("\nouter rim MUST fade:\n");
    chk(lum(m, 1, 19) < 60,    "P0 left rim (x=1) is strongly faded");
    chk(lum(m, 40, 3) < 60,    "P0 top rim (y=3) is strongly faded");
    chk(lum(m, 158, 19) < 60,  "P1 right rim (x=158) is strongly faded");
    chk(lum(m, 40, 66) < 60,   "P2 bottom rim (y=66) is strongly faded");
    std::printf("\nedges facing the 30px gap are rim too (they really cut off):\n");
    chk(lum(m, 64, 19) < 60,   "P0 right edge (x=64, faces gap) is faded");
    chk(lum(m, 95, 19) < 60,   "P1 left edge (x=95, faces gap) is faded");

    std::printf("\nramp shape:\n");
    chk(lum(m, 1, 19) < lum(m, 4, 19) && lum(m, 4, 19) < lum(m, 8, 19),
        "brightness increases monotonically inward from the rim");
    chk(lum(m, 32, 19) == 255, "deep interior is untouched");
    chk(lum(m, 9, 19) == 255,  "full brightness reached by the ramp width");

    std::printf("\noutside the panel union:\n");
    chk(lum(m, 80, 10) == 0,   "gap column between the top panels is black");
    chk(lum(m, 5, 50) == 0,    "notch left of P2 (outside any panel) is black");

    std::printf("\ndisabled / degenerate:\n");
    {
        cv::Mat z(canvas_sz, CV_8UC3, cv::Scalar(255, 255, 255));
        face::EdgeFade ef; ef.apply(z, panels, 0, true);
        chk(lum(z, 1, 19) == 255, "width 0 is a true no-op");
    }
    {
        // No panels configured (single-panel / preview build) -> canvas rect.
        cv::Mat z(canvas_sz, CV_8UC3, cv::Scalar(255, 255, 255));
        face::EdgeFade ef; ef.apply(z, {}, W, false);
        chk(lum(z, 0, 35) < 60 && lum(z, 80, 35) == 255,
            "empty panel list falls back to the canvas rectangle");
    }

    std::printf("\ndither:\n");
    {
        cv::Mat d = run(/*dither=*/true);
        // Same ramp, but neighbouring pixels at equal depth should differ.
        bool varies = false;
        for (int x = 2; x < 8 && !varies; ++x)
            for (int y = 12; y < 26; ++y)
                if (d.at<cv::Vec3b>(y, x)[0] != d.at<cv::Vec3b>(y + 1, x)[0])
                    { varies = true; break; }
        chk(varies, "dithered ramp varies between adjacent rows (stipple)");
        chk(d.at<cv::Vec3b>(19, 32)[0] == 255, "dither leaves the interior alone");
        chk(d.at<cv::Vec3b>(10, 80)[0] == 0,   "dither leaves outside-union black");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
