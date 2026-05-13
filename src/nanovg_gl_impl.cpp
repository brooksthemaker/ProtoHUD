// Compile the NanoVG GLES2 backend into the protohud binary.
// Include order matters: nanovg.h defines NVGcontext (used in nanovg_gl.h's
// public declarations), GLES2/gl2.h provides GL types for the implementation.
#include <nanovg.h>
#include <GLES2/gl2.h>
#define NANOVG_GLES2_IMPLEMENTATION
#include <nanovg_gl.h>
