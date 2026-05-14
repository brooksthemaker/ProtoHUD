#pragma once
#include <string>

enum class CaptureRequest : uint8_t { None, Left, Right, Stereo };

struct AppState;
class  XRDisplay;

// Called on the render thread: reads pixel data from the camera eye FBOs,
// writes a PNG to dir asynchronously, and pushes a toast via state.notifs.
// Resets state.capture_request = None before returning.
void do_capture(CaptureRequest req, XRDisplay& xr,
                const std::string& dir, AppState& state);
