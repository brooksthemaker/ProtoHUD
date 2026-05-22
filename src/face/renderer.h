#pragma once
// ── renderer.h ─────────────────────────────────────────────────────────────────
// C++ port of protoface/renderer.py. Stateless compositing helpers over RGB(A)
// cv::Mat buffers (channel 0 = R). Sizes are derived from the Mats, so unlike the
// Python Renderer there's no per-size object to cache.

#include <vector>
#include <cstdint>
#include <opencv2/core.hpp>

namespace face {

enum class Blend { Normal, Add };

struct Layer {
    cv::Mat rgba;          // (h, w) CV_8UC4 RGBA; empty = skipped
    Blend   blend = Blend::Normal;
};

// (h, w) CV_8UC3 filled with an RGB colour.
cv::Mat solid_layer(uint8_t r, uint8_t g, uint8_t b, int width, int height);

// Combine a face sprite (RGBA; RGB=shading, A=mask) with a material colour layer
// (RGB): material × normalised face luminance, alpha preserved. Returns RGBA.
cv::Mat apply_material(const cv::Mat& face_rgba, const cv::Mat& material_rgb);

// Alpha-composite layers (bottom-to-top) over an RGB base. Returns RGB.
cv::Mat composite(const cv::Mat& base_rgb, const std::vector<Layer>& layers);

// Scale an RGB frame's brightness by value/255 in place-safe fashion.
cv::Mat scale_brightness(const cv::Mat& rgb, int value /*0-255*/);

} // namespace face
