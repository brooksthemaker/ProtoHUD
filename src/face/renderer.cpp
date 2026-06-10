#include "renderer.h"

#include <opencv2/imgproc.hpp>

namespace face {

cv::Mat solid_layer(uint8_t r, uint8_t g, uint8_t b, int width, int height) {
    return cv::Mat(height, width, CV_8UC3,
                   cv::Scalar(static_cast<double>(r),
                              static_cast<double>(g),
                              static_cast<double>(b)));
}

cv::Mat apply_material(const cv::Mat& face_rgba, const cv::Mat& material_rgb) {
    // Scratch buffers persisted across calls so the steady state allocates
    // nothing (cv create() is a no-op when size/type already match) — this
    // runs per panel per tick and used to make ~10 full-canvas heap
    // temporaries each time. thread_local because only the face render
    // thread calls this (NativeFaceController::render_thread); the returned
    // Mat shares `result`, which is safe as callers consume it within the
    // same tick before the next call.
    static thread_local std::vector<cv::Mat> f;    // face R,G,B,A (uint8)
    static thread_local std::vector<cv::Mat> m;    // material R,G,B (uint8)
    static thread_local std::vector<cv::Mat> outv;
    static thread_local cv::Mat r, g, b, lum, mr, mg, mb, cr, cg, cb, result;

    cv::split(face_rgba, f);

    f[0].convertTo(r, CV_32F);
    f[1].convertTo(g, CV_32F);
    f[2].convertTo(b, CV_32F);
    // lum = (r + g + b) / 3 / 255 — (h,w) float, 0..1
    cv::add(r, g, lum);
    cv::add(lum, b, lum);
    lum *= 1.0f / (3.0f * 255.0f);

    cv::split(material_rgb, m);
    m[0].convertTo(mr, CV_32F);
    m[1].convertTo(mg, CV_32F);
    m[2].convertTo(mb, CV_32F);

    // colour × luminance, saturate to 0..255 (multiply in place — mr/mg/mb
    // are scratch and not used again this call)
    cv::multiply(mr, lum, mr);
    cv::multiply(mg, lum, mg);
    cv::multiply(mb, lum, mb);
    mr.convertTo(cr, CV_8U);
    mg.convertTo(cg, CV_8U);
    mb.convertTo(cb, CV_8U);

    outv.assign({cr, cg, cb, f[3]});   // Mat header copies, no pixel copies
    cv::merge(outv, result);
    return result;                    // CV_8UC4 RGBA
}

cv::Mat composite(const cv::Mat& base_rgb, const std::vector<Layer>& layers) {
    // Same scratch-reuse scheme as apply_material (render thread only; zero
    // steady-state allocations). The returned Mat shares `res` — callers
    // consume it before the next composite() call.
    static thread_local std::vector<cv::Mat> ch;     // layer R,G,B,A (uint8)
    static thread_local std::vector<cv::Mat> rgbv, a3v;
    static thread_local cv::Mat out, rgb, rgbf, af, a3, inv, res;

    base_rgb.convertTo(out, CV_32FC3);

    for (const Layer& L : layers) {
        if (L.rgba.empty()) continue;

        cv::split(L.rgba, ch);

        rgbv.assign({ch[0], ch[1], ch[2]});                // header copies only
        cv::merge(rgbv, rgb);
        rgb.convertTo(rgbf, CV_32FC3);

        ch[3].convertTo(af, CV_32F, 1.0 / 255.0);          // (h,w) 0..1
        a3v.assign({af, af, af});
        cv::merge(a3v, a3);                                 // (h,w,3)

        // rgbf is scratch — premultiply by alpha in place.
        cv::multiply(rgbf, a3, rgbf);
        if (L.blend == Blend::Add) {
            cv::add(out, rgbf, out);                        // out += src·a
        } else {
            cv::subtract(cv::Scalar::all(1.0), a3, inv);
            cv::multiply(out, inv, out);                    // out = out·(1-a) + src·a
            cv::add(out, rgbf, out);
        }
    }

    out.convertTo(res, CV_8UC3);      // saturating cast == np.clip(...).astype
    return res;
}

cv::Mat scale_brightness(const cv::Mat& rgb, int value) {
    if (value >= 255) return rgb;
    if (value <= 0)   return cv::Mat::zeros(rgb.size(), rgb.type());
    cv::Mat out;
    rgb.convertTo(out, CV_8UC3, value / 255.0);
    return out;
}

} // namespace face
