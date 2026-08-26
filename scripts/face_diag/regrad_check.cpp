// Check the gradient-spec rewriter: that angle/speed overrides land in the right
// fields, that the mirror flag survives (every built-in gradient depends on it
// for left/right face symmetry), and that the defaults reproduce the shipped
// spec exactly.
#include <cstdio>
#include <string>

// Copy of the helper under test (file-local static in native_face_controller.cpp).
static std::string regrad_spec(const std::string& spec,
                               const int* angle, const int* speed) {
    if (spec.rfind("gradient:", 0) != 0) return spec;
    const size_t p0 = 9;
    const size_t c1 = spec.find(':', p0);
    if (c1 == std::string::npos) return spec;
    const size_t c2 = spec.find(':', c1 + 1);
    if (c2 == std::string::npos) return spec;
    const size_t c3 = spec.find(':', c2 + 1);
    if (c3 == std::string::npos) return spec;
    std::string dir  = spec.substr(p0, c1 - p0);
    std::string mode = spec.substr(c1 + 1, c2 - c1 - 1);
    std::string spd  = spec.substr(c2 + 1, c3 - c2 - 1);
    const std::string stops = spec.substr(c3 + 1);
    if (angle) {
        const bool mirror = !dir.empty() && (dir.back() == 'm' || dir.back() == 'M');
        const int  a      = ((*angle % 360) + 360) % 360;
        dir = "a" + std::to_string(a) + (mirror ? "m" : "");
    }
    if (speed) spd = std::to_string(*speed);
    return "gradient:" + dir + ":" + mode + ":" + spd + ":" + stops;
}

static bool fail = false;
static void eq(const std::string& got, const std::string& want,
               const std::string& what) {
    const bool ok = got == want;
    std::printf("  %-46s %s\n", what.c_str(), ok ? "PASS" : "*** FAIL ***");
    if (!ok) std::printf("      got  %s\n      want %s\n", got.c_str(), want.c_str());
    if (!ok) fail = true;
}

int main() {
    const std::string lava  = "gradient:hm:s:0:2A0A0A-C81E00-FF8C00";  // preset 17
    const std::string pride = "gradient:v:s:0:FF0000-00FF00-0000FF";   // flag shape
    const int zero = 0, a45 = 45, a90 = 90, neg = -30, s12 = 12, sneg = -20;

    std::printf("defaults reproduce the shipped look\n");
    eq(regrad_spec(lava, &zero, &zero), "gradient:a0m:s:0:2A0A0A-C81E00-FF8C00",
       "angle 0 + speed 0 -> a0m (== hm), still static");

    std::printf("\nangle\n");
    eq(regrad_spec(lava, &a45, nullptr), "gradient:a45m:s:0:2A0A0A-C81E00-FF8C00",
       "45 deg KEEPS the mirror (m)");
    eq(regrad_spec(lava, &a90, nullptr), "gradient:a90m:s:0:2A0A0A-C81E00-FF8C00",
       "90 deg keeps the mirror");
    eq(regrad_spec(pride, &a45, nullptr), "gradient:a45:s:0:FF0000-00FF00-0000FF",
       "an unmirrored spec does NOT gain a mirror");
    eq(regrad_spec(lava, &neg, nullptr), "gradient:a330m:s:0:2A0A0A-C81E00-FF8C00",
       "negative angle normalises to 0-359");

    std::printf("\nspeed\n");
    eq(regrad_spec(lava, nullptr, &s12), "gradient:hm:s:12:2A0A0A-C81E00-FF8C00",
       "speed lands in field 3, direction untouched");
    eq(regrad_spec(lava, nullptr, &sneg), "gradient:hm:s:-20:2A0A0A-C81E00-FF8C00",
       "negative speed survives (reverses the flow)");
    eq(regrad_spec(regrad_spec(lava, &a90, nullptr), nullptr, &s12),
       "gradient:a90m:s:12:2A0A0A-C81E00-FF8C00",
       "angle then speed compose (the real call order)");

    std::printf("\nnon-gradient specs pass through untouched\n");
    eq(regrad_spec("solid:255,0,0", &a45, &s12), "solid:255,0,0", "solid");
    eq(regrad_spec("teal", &a45, &s12), "teal", "named colour");
    eq(regrad_spec("rainbow.png", &a45, &s12), "rainbow.png", "PNG pattern asset");

    std::printf("\nmalformed specs are left alone, not corrupted\n");
    eq(regrad_spec("gradient:hm", &a45, &s12), "gradient:hm", "too few fields");
    eq(regrad_spec("gradient:hm:s", &a45, &s12), "gradient:hm:s", "no speed/stops");
    eq(regrad_spec("gradient:hm:s:0", &a45, &s12), "gradient:hm:s:0",
       "no stop list");

    std::printf("\nbanded mode (pride sharp bands) is preserved\n");
    eq(regrad_spec("gradient:v:b:0:FF0000-0000FF", &a45, &s12),
       "gradient:a45:b:12:FF0000-0000FF", "mode field untouched");

    std::printf("\n%s\n", fail ? "*** SOME CHECKS FAILED ***" : "ALL CHECKS PASSED");
    return fail ? 1 : 0;
}
