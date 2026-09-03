#pragma once
// Face-layer dithering: reduce the composited canvas to the panel's REAL
// colour depth using an ordered pattern, so gradients break into a fine
// stipple instead of stepping into visible bands.
//
// Why this is needed at all: piomatter drives the panels with `n_planes` PWM
// bit planes, so a 6-plane build shows 64 levels per channel, not 256. The
// renderer works in 8-bit and hands over values the panel simply cannot
// represent, and the hardware truncates them — which is what puts hard bands
// across a smooth gradient. Doing the reduction here, with a dither, spends
// the same 64 levels but spreads the error spatially.
//
// ⚠ Only meaningful when the panel depth is BELOW 8 bits. At 8 planes or more
// there is nothing to reduce and this is a no-op by construction.

#include "dither.h"

#include <opencv2/opencv.hpp>

namespace face {

class FaceDither {
public:
    // planes: panel PWM bit planes (levels = 1 << planes). <= 0 or >= 8 is a
    // no-op — see the note above.
    static void apply(cv::Mat& canvas, int planes) {
        if (canvas.empty() || canvas.type() != CV_8UC3) return;
        if (planes <= 0 || planes >= 8) return;
        const int levels = 1 << planes;
        for (int y = 0; y < canvas.rows; ++y) {
            cv::Vec3b* p = canvas.ptr<cv::Vec3b>(y);
            for (int x = 0; x < canvas.cols; ++x)
                for (int c = 0; c < 3; ++c)
                    p[x][c] = dither_channel(p[x][c], levels, x, y);
        }
    }
};

}  // namespace face
