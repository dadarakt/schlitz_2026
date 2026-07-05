// Sync Pulse pattern -- every LED (every bar + the matrix) hard-cuts
// between fully on (one color randomly sampled from the palette) and
// fully off, on a 50% duty cycle, all in lockstep. Built primarily as a
// quick, obvious visual check for mesh-sync testing: both the color pick
// and the on/off phase are derived from deterministic_rand/
// quantize_sync_ms (see shared.h) rather than plain rand()/local time, so
// multiple boards sharing a synced clock flip on and off at the exact
// same instant, in the exact same color. A hard cut (rather than a smooth
// fade) is deliberate -- it makes any timing mismatch between boards
// trivially visible as a gap between one board's LEDs switching and the
// other's, instead of a subtle blur a fade could mask.
//
// The color re-rolls every SP_COLOR_RECHECK_CYCLES pulses instead of once
// when the pattern starts. That's deliberate too: this pattern typically
// becomes active immediately at boot (it's the default), before the mesh
// clock sync has necessarily completed -- a single pick made then could
// land on different colors on different boards with no way to recover.
// Re-rolling periodically means even a mismatched first guess self-heals
// once the clock is actually synced, and stays converged from then on
// (each re-roll seeds from the previous agreed cycle boundary, never from
// "now"). Crucially, the re-roll is computed from the pulse's own cycle
// count, so it only ever lands exactly on an off->on boundary -- never
// mid-pulse, which would look like a glitch.

#include "shared.h"
#include <led_viz.h>

#define SP_PERIOD_MS 1000.0 // one full on+off cycle
#define SP_COLOR_RECHECK_CYCLES 8 // re-roll color every N cycles (~8s)

static RGB sp_color = {0, 0, 0};
static long sp_last_color_window = -1;

static void sp_init(void) {
    sp_color = (RGB){0, 0, 0};
    sp_last_color_window = -1;
}

static void sync_pulse_update(double time_ms, PixelFunc pixel,
                              const Palette16 palette) {
    long cycle = (long)(time_ms / SP_PERIOD_MS);
    long color_window = cycle / SP_COLOR_RECHECK_CYCLES;

    if (color_window != sp_last_color_window) {
        // Exact start-of-window boundary timestamp -- a clean multiple of
        // the pulse period, so re-rolling here can only ever coincide with
        // an off->on transition, never fall mid-pulse.
        double seed = (double)(color_window * SP_COLOR_RECHECK_CYCLES) * SP_PERIOD_MS;
        uint8_t hue = (uint8_t)(deterministic_rand(seed, 1) & 0xFF);
        sp_color = palette_sample(palette, hue, 255, true);
        sp_last_color_window = color_window;
    }

    double phase = fmod(time_ms, SP_PERIOD_MS) / SP_PERIOD_MS; // 0..1
    bool on = phase < 0.5;
    RGB c = on ? sp_color : (RGB){0, 0, 0};

    int n_strips = get_num_strips();
    for (int s = 0; s < n_strips; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            pixel(s, i, &c.r, &c.g, &c.b);
        }
    }
}
