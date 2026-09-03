// Headless check for face/face_dither.h — proves the depth reduction actually
// breaks banding rather than just moving it.
//
// build: g++ -std=c++17 -O2 -I src scripts/face_diag/face_dither_check.cpp \
//            $(pkg-config --cflags --libs opencv4) -o /tmp/face_dither_check
#include "face/face_dither.h"
#include <cstdio>
#include <set>

static int fails = 0;
static void chk(bool ok, const char* what) {
    std::printf("  %-56s %s\n", what, ok ? "PASS" : "*** FAIL ***");
    if (!ok) ++fails;
}

// Count horizontal runs of constant value along the middle row — a "band" is a
// long run. Fewer/shorter runs of flat colour == smoother-looking gradient.
static int longest_flat_run(const cv::Mat& m) {
    const int y = m.rows / 2;
    int best = 1, cur = 1;
    for (int x = 1; x < m.cols; ++x) {
        if (m.at<cv::Vec3b>(y, x) == m.at<cv::Vec3b>(y, x - 1)) { if (++cur > best) best = cur; }
        else cur = 1;
    }
    return best;
}

int main() {
    const int W = 256, H = 32, PLANES = 6;          // this rig's camera_planes
    // A smooth 0..255 horizontal ramp — the classic banding case.
    cv::Mat ramp(H, W, CV_8UC3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uchar v = static_cast<uchar>(x * 255 / (W - 1));
            ramp.at<cv::Vec3b>(y, x) = cv::Vec3b(v, v, v);
        }

    // What the panel does today: truncate to 64 levels, no dither.
    cv::Mat trunc = ramp.clone();
    const int levels = 1 << PLANES;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) {
                int q = trunc.at<cv::Vec3b>(y, x)[c] * (levels - 1) / 255;
                trunc.at<cv::Vec3b>(y, x)[c] =
                    static_cast<uchar>(q * 255 / (levels - 1));
            }

    cv::Mat dith = ramp.clone();
    face::FaceDither::apply(dith, PLANES);

    std::printf("face_dither_check — %d planes (%d levels), %dpx ramp\n\n",
                PLANES, levels, W);

    const int run_trunc = longest_flat_run(trunc);
    const int run_dith  = longest_flat_run(dith);
    std::printf("banding (longest flat run along the ramp):\n");
    std::printf("    truncated (what the panel does now): %d px\n", run_trunc);
    std::printf("    dithered                           : %d px\n\n", run_dith);
    chk(run_dith <= run_trunc, "full-range ramp: dither does not make it worse");
    std::printf("    (a full-range ramp is already ~%d px per level, so there is\n"
                "     little banding here to break — the real case is below)\n\n",
                W / levels);

    // THE CASE THAT ACTUALLY BANDS: a shallow gradient. A face's material
    // ramps across a narrow slice of the range, so each surviving level spans
    // many pixels and truncation lays down wide flat bars.
    cv::Mat shallow(H, W, CV_8UC3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uchar v = static_cast<uchar>(100 + x * 20 / (W - 1));  // 100..120
            shallow.at<cv::Vec3b>(y, x) = cv::Vec3b(v, v, v);
        }
    cv::Mat sh_trunc = shallow.clone();
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 3; ++c) {
                int q = sh_trunc.at<cv::Vec3b>(y, x)[c] * (levels - 1) / 255;
                sh_trunc.at<cv::Vec3b>(y, x)[c] =
                    static_cast<uchar>(q * 255 / (levels - 1));
            }
    cv::Mat sh_dith = shallow.clone();
    face::FaceDither::apply(sh_dith, PLANES);
    const int sh_run_t = longest_flat_run(sh_trunc);
    const int sh_run_d = longest_flat_run(sh_dith);
    std::printf("shallow gradient (100..120 over %dpx) — the real banding case:\n", W);
    std::printf("    truncated: %d px flat bars\n", sh_run_t);
    std::printf("    dithered : %d px\n", sh_run_d);
    chk(sh_run_d * 2 < sh_run_t, "dither more than halves the flat-run length");
    chk(sh_run_t > 20,           "...and truncation really does band that badly");

    // A single row understates it: neighbouring ROWS get different Bayer
    // thresholds, so the stipple is a 2D structure. What the eye reads as a
    // band is a patch that is flat in BOTH axes, so measure it that way — the
    // share of 8x8 tiles carrying more than one level.
    auto mixed_tiles = [](const cv::Mat& m) {
        int mixed = 0, total = 0;
        for (int ty = 0; ty + 8 <= m.rows; ty += 8)
            for (int tx = 0; tx + 8 <= m.cols; tx += 8) {
                std::set<int> s;
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        s.insert(m.at<cv::Vec3b>(ty + y, tx + x)[0]);
                if (s.size() > 1) ++mixed;
                ++total;
            }
        return total ? (100 * mixed / total) : 0;
    };
    const int mt_t = mixed_tiles(sh_trunc), mt_d = mixed_tiles(sh_dith);
    std::printf("    8x8 tiles carrying more than one level:"
                "  truncated %d%%   dithered %d%%\n", mt_t, mt_d);
    chk(mt_d >= 90, "dithered: nearly every tile is a blend, not a flat patch");
    chk(mt_t <= 25, "truncated: most tiles are a single flat level");

    std::printf("\ndepth is genuinely respected:\n");
    auto distinct = [](const cv::Mat& m) {
        std::set<int> s;
        for (int y = 0; y < m.rows; ++y)
            for (int x = 0; x < m.cols; ++x) s.insert(m.at<cv::Vec3b>(y, x)[0]);
        return static_cast<int>(s.size());
    };
    chk(distinct(dith) <= levels,
        "output uses no more levels than the panel can show");
    chk(distinct(dith) > 1, "output is not collapsed to a single level");

    std::printf("\nfidelity (dither must not shift the picture):\n");
    double sum_r = 0, sum_d = 0;
    for (int x = 0; x < W; ++x) { sum_r += ramp.at<cv::Vec3b>(H/2, x)[0];
                                  sum_d += dith.at<cv::Vec3b>(H/2, x)[0]; }
    const double err = std::abs(sum_r - sum_d) / W;
    std::printf("    mean error across the ramp: %.2f/255\n", err);
    chk(err < 2.0, "mean brightness preserved (<2/255 drift)");

    std::printf("\nstability + no-ops:\n");
    {
        cv::Mat a = ramp.clone(), b = ramp.clone();
        face::FaceDither::apply(a, PLANES);
        face::FaceDither::apply(b, PLANES);
        chk(cv::countNonZero(cv::Mat(cv::sum(cv::abs(a - b)))) == 0,
            "deterministic — same input gives the same stipple every frame");
    }
    {
        cv::Mat z = ramp.clone();
        face::FaceDither::apply(z, 8);
        chk(cv::norm(z, ramp, cv::NORM_INF) == 0, "8 planes is a true no-op");
        face::FaceDither::apply(z, 0);
        chk(cv::norm(z, ramp, cv::NORM_INF) == 0, "0 planes (off) is a true no-op");
    }
    {
        cv::Mat flat(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
        face::FaceDither::apply(flat, PLANES);
        chk(cv::norm(flat, cv::NORM_INF) == 0, "pure black stays pure black");
        cv::Mat white(H, W, CV_8UC3, cv::Scalar(255, 255, 255));
        face::FaceDither::apply(white, PLANES);
        chk(white.at<cv::Vec3b>(0,0) == cv::Vec3b(255,255,255),
            "pure white stays pure white");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
