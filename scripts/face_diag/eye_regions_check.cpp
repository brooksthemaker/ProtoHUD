// Headless check for PER-EXPRESSION eye regions ("eye_regions" in a face
// folder's config.json). Builds a throwaway face folder whose shared pair and
// per-expression override sit in different quadrants, runs the real
// FaceLoader + FaceState with eyes closed, and reads back WHERE the blink art
// landed — so the override/fallback choice is verified by what comes out.
//
// build: g++ -std=c++17 -O2 -I src scripts/face_diag/eye_regions_check.cpp \
//            src/face/face_loader.cpp src/face/face_state.cpp \
//            $(pkg-config --cflags --libs opencv4) -o /tmp/eye_regions_check
#include "face/face_loader.h"
#include "face/face_state.h"
#include "face/face_config.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int fails = 0;
static void chk(bool ok, const std::string& what) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) ++fails;
}

static const int W = 32, H = 16;

static void write_png(const fs::path& p, int r, int g, int b) {
    fs::create_directories(p.parent_path());
    cv::Mat m(H, W, CV_8UC4, cv::Scalar(b, g, r, 255));   // imwrite wants BGRA
    cv::imwrite(p.string(), m);
}

// Shared pair = TOP quadrants, happy's override = BOTTOM quadrants — so one
// pixel read tells which pair masked the blink.
static const char* kSharedBoxes =
    "\"eye_left\":{\"points\":[[2,2],[13,2],[13,6],[2,6]]},"
    "\"eye_right\":{\"points\":[[18,2],[29,2],[29,6],[18,6]]}";
static const char* kHappyBoxes =
    "\"eye_regions\":{\"happy\":{"
    "\"eye_left\":{\"points\":[[2,9],[13,9],[13,13],[2,13]]},"
    "\"eye_right\":{\"points\":[[18,9],[29,9],[29,13],[18,13]]}}}";

static fs::path make_face(const std::string& name, bool with_override) {
    const fs::path dir = fs::temp_directory_path() / ("eyeregchk_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    write_png(dir / "neutral.png", 200, 200, 200);
    write_png(dir / "happy.png",   200, 200, 200);
    write_png(dir / "wink.png",    200, 200, 200);        // no override of its own
    write_png(dir / "blink.png",   255,   0,   0);        // red = the blink art
    std::ofstream(dir / "config.json")
        << "{" << kSharedBoxes
        << (with_override ? std::string(",") + kHappyBoxes : std::string())
        << "}";
    return dir;
}

// Render one closed-eye frame of `expr` and return the red channel at four
// probe points: shared-left box, happy-left box, between the boxes, corner.
struct Probe { int shared_box, happy_box, between, corner; };
static Probe closed_frame(const fs::path& dir, const std::string& expr) {
    face::FaceLoader loader(dir.string(), W, H);
    face::FaceCfg fcfg;
    face::FaceState st(fcfg, loader.expression_names());
    st.set_expression(expr);
    for (int i = 0; i < 120; ++i) st.update(1.0 / 60.0);  // finish the crossfade
    st.set_eyes_closed(true);
    st.update(1.0 / 60.0);
    cv::Mat f = loader.get_frame(st);
    auto red = [&](int y, int x){ return static_cast<int>(f.at<cv::Vec4b>(y, x)[0]); };
    return { red(4, 6), red(11, 6), red(8, 15), red(0, 0) };
}

int main() {
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
    const int kFace = 200, kBlink = 255;

    std::printf("[1] override present: each expression closes its own boxes\n");
    {
        const fs::path dir = make_face("override", true);
        const Probe n = closed_frame(dir, "neutral");
        chk(n.shared_box == kBlink, "neutral: blink lands in the shared boxes");
        chk(n.happy_box  == kFace,  "neutral: happy's boxes untouched");
        chk(n.between == kFace && n.corner == kFace,
            "neutral: nothing outside any box");
        const Probe h = closed_frame(dir, "happy");
        chk(h.happy_box  == kBlink, "happy: blink lands in ITS OWN boxes");
        chk(h.shared_box == kFace,  "happy: shared boxes untouched");
        chk(h.between == kFace && h.corner == kFace,
            "happy: nothing outside any box");
        const Probe w = closed_frame(dir, "wink");
        chk(w.shared_box == kBlink && w.happy_box == kFace,
            "wink (no override): falls back to the shared boxes");

        face::FaceLoader loader(dir.string(), W, H);
        const cv::Mat& shared = loader.eye_region_mask();
        chk(loader.eye_region_mask("happy").data != shared.data,
            "mask accessor: happy returns its own stencil");
        chk(loader.eye_region_mask("wink").data == shared.data,
            "mask accessor: wink returns the shared stencil");
        chk(loader.eye_region_mask("happy").at<uint8_t>(11, 6) == 255 &&
            loader.eye_region_mask("happy").at<uint8_t>(4, 6) == 0,
            "happy's stencil covers its boxes, not the shared ones");
        chk(loader.eye_lid_line("happy").bottom[6] != loader.eye_lid_line().bottom[6],
            "lid line: happy's lower edge differs from the shared one");
        fs::remove_all(dir);
    }

    std::printf("[2] no eye_regions key: legacy behaviour byte-identical\n");
    {
        const fs::path dir = make_face("legacy", false);
        const Probe h = closed_frame(dir, "happy");
        chk(h.shared_box == kBlink && h.happy_box == kFace,
            "happy without override blinks in the shared boxes");
        face::FaceLoader loader(dir.string(), W, H);
        chk(loader.eye_region_mask("happy").data == loader.eye_region_mask().data,
            "mask accessor falls back to the shared stencil");
        fs::remove_all(dir);
    }

    std::printf("%s\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return fails ? 1 : 0;
}
