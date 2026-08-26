// Do Gradient, Wave, Chase and the sound pulses all TRAVEL THE SAME WAY for a
// positive rate? Measures each pattern's motion by cross-correlating one frame
// against a later one and reporting which way the feature moved along lf.
//
// Direction is measured, not derived: a still frame can't show travel, and the
// algebra is exactly what was wrong before.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static const int    N  = 200;      // samples along the length
static const double DT = 0.25;     // frame gap (big enough to beat the 1/N grid)

// ── the four pattern kernels, copied from accessory_leds.cpp ────────────────
static double grad(double lf, double t, double v, bool linked) {
    double m = lf;
    if (v != 0.0) {
        double f = lf - v * t;              // the fix under test
        f -= std::floor(f);
        m = linked ? f : (1.0 - std::fabs(2.0 * f - 1.0));
    }
    return m;                                // "colour" scalar
}
static double wave(double lf, double t, double v, bool linked) {
    double head = std::fmod(v * t, 1.0);
    if (head < 0) head += 1.0;
    double d = std::fabs(lf - head);
    if (!linked) d = std::min(d, 1.0 - d);
    double px = std::max(0.0, 1.0 - d / 0.18);
    return px * px;
}
static double chase(double lf, double t, double rate) {
    double head = std::fmod(rate * t, 1.0);
    if (head < 0) head += 1.0;
    double d = head - lf;
    if (d < 0) d += 1.0;
    return std::max(0.0, 1.0 - d / 0.35);
}
// Sound pulse: a head advanced by wave_speed*dt, band |lf - head|.
static double pulse(double lf, double t, double v) {
    double head = std::fmod(v * t, 1.0);
    if (head < 0) head += 1.0;
    const double d = std::fabs(lf - head);
    const double b = std::max(0.0, 1.0 - d / 0.18);
    return b * b;
}

// Shift (in samples) that best aligns frame(t) onto frame(t+DT). Positive =
// the feature moved toward HIGHER lf (base -> tip).
template <typename F>
static int travel(F f) {
    std::vector<double> a(N), b(N);
    const double t0 = 1.0;
    for (int i = 0; i < N; ++i) {
        const double lf = double(i) / (N - 1);
        a[i] = f(lf, t0);
        b[i] = f(lf, t0 + DT);
    }
    int    best_s = 0;
    double best_c = -1e18;
    for (int s = -N / 2; s <= N / 2; ++s) {
        double c = 0;
        int    n = 0;
        for (int i = 0; i < N; ++i) {
            const int j = i + s;
            if (j < 0 || j >= N) continue;
            c += a[i] * b[j];
            ++n;
        }
        if (n < N / 3) continue;
        c /= n;                              // normalise so edge shifts don't win
        if (c > best_c) { best_c = c; best_s = s; }
    }
    return best_s;
}

static const char* dir(int s) {
    return s > 2 ? "base->tip" : s < -2 ? "tip->base" : "(no clear motion)";
}

int main() {
    const double v = 0.6;      // positive Wave Speed / Breathe Rate
    struct Row { const char* name; int shift; };
    std::vector<Row> rows;

    for (bool linked : { true, false }) {
        std::printf("\n%s zones, positive rate (%.2f)\n",
                    linked ? "LINKED (hub+fin)" : "STANDALONE", v);
        rows.clear();
        rows.push_back({ "Gradient", travel([&](double lf, double t){
            return grad(lf, t, v, linked); }) });
        rows.push_back({ "Wave", travel([&](double lf, double t){
            return wave(lf, t, v, linked); }) });
        rows.push_back({ "Chase", travel([&](double lf, double t){
            return chase(lf, t, v); }) });
        rows.push_back({ "Sound pulse", travel([&](double lf, double t){
            return pulse(lf, t, v); }) });

        for (const auto& r : rows)
            std::printf("  %-12s shift %+4d  %s\n", r.name, r.shift, dir(r.shift));

        // The ping-pong gradient folds, so only its SIGN over a short window is
        // meaningful; report agreement on sign.
        bool agree = true;
        const int ref = rows[0].shift;
        for (const auto& r : rows)
            if ((r.shift > 0) != (ref > 0)) agree = false;
        std::printf("  => %s\n", agree ? "ALL FOUR TRAVEL THE SAME WAY"
                                       : "*** DIRECTIONS DISAGREE ***");
    }

    // Control: with the OLD `+` gradient, the mismatch must be visible — proving
    // the test discriminates rather than always agreeing.
    std::printf("\nCONTROL: old gradient (lf + v*t), linked\n");
    const int old_g = travel([&](double lf, double t){
        double f = lf + v * t; f -= std::floor(f); return f; });
    const int new_g = travel([&](double lf, double t){ return grad(lf, t, v, true); });
    const int w     = travel([&](double lf, double t){ return wave(lf, t, v, true); });
    std::printf("  old Gradient shift %+4d  %s\n", old_g, dir(old_g));
    std::printf("  new Gradient shift %+4d  %s\n", new_g, dir(new_g));
    std::printf("  Wave         shift %+4d  %s\n", w, dir(w));
    std::printf("  => %s\n",
        ((old_g > 0) != (w > 0) && (new_g > 0) == (w > 0))
        ? "old disagreed with Wave, new agrees - fix confirmed"
        : "*** control failed: the test does not discriminate ***");
    return 0;
}
