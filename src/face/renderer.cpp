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
    std::vector<cv::Mat> f;            // R,G,B,A (uint8)
    cv::split(face_rgba, f);

    cv::Mat r, g, b;
    f[0].convertTo(r, CV_32F);
    f[1].convertTo(g, CV_32F);
    f[2].convertTo(b, CV_32F);
    cv::Mat lum = (r + g + b) / 3.0f / 255.0f;   // (h,w) float, 0..1

    std::vector<cv::Mat> m;            // material R,G,B (uint8)
    cv::split(material_rgb, m);
    cv::Mat mr, mg, mb;
    m[0].convertTo(mr, CV_32F);
    m[1].convertTo(mg, CV_32F);
    m[2].convertTo(mb, CV_32F);

    cv::Mat cr, cg, cb;               // colour × luminance, saturate to 0..255
    mr.mul(lum).convertTo(cr, CV_8U);
    mg.mul(lum).convertTo(cg, CV_8U);
    mb.mul(lum).convertTo(cb, CV_8U);

    std::vector<cv::Mat> out{cr, cg, cb, f[3]};
    cv::Mat result;
    cv::merge(out, result);
    return result;                    // CV_8UC4 RGBA
}

cv::Mat composite(const cv::Mat& base_rgb, const std::vector<Layer>& layers) {
    cv::Mat out;
    base_rgb.convertTo(out, CV_32FC3);

    for (const Layer& L : layers) {
        if (L.rgba.empty()) continue;

        std::vector<cv::Mat> ch;      // R,G,B,A (uint8)
        cv::split(L.rgba, ch);

        cv::Mat rgb;
        cv::merge(std::vector<cv::Mat>{ch[0], ch[1], ch[2]}, rgb);
        cv::Mat rgbf;
        rgb.convertTo(rgbf, CV_32FC3);

        cv::Mat af;
        ch[3].convertTo(af, CV_32F, 1.0 / 255.0);          // (h,w) 0..1
        cv::Mat a3;
        cv::merge(std::vector<cv::Mat>{af, af, af}, a3);    // (h,w,3)

        if (L.blend == Blend::Add) {
            out = out + rgbf.mul(a3);
        } else {
            cv::Mat inv;
            cv::subtract(cv::Scalar::all(1.0), a3, inv);
            out = out.mul(inv) + rgbf.mul(a3);
        }
    }

    cv::Mat res;
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
