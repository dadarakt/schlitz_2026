// WS2801 ambient overlay -- the two WS2801 strips (indices 5, 6) don't
// follow whichever program is currently active; instead they run their own
// occasional background events on top of it, cycling between three modes
// (a random, deliberately long duration each, so changes are occasional
// not frequent):
//   - shooting star: a bright point travels the strip's length once,
//     leaving a fading trail behind it, then waits a random gap before
//     the next one launches.
//   - sparkle: white twinkles across the strip, as one rare burst per
//     mode activation -- density ramps up from near-nothing to a lot of
//     sparkles, then fades back down, rather than a constant rate.
//   - pulse: the whole strip breathes gently in one palette color --
//     slight, not a hard on/off cut.
// The two strips run this independently (own random timing/color each),
// which reads as more organic for a background effect than keeping them
// in lockstep.
//
// This is local to a single board -- there's only ever one board with
// WS2801 hardware -- so none of the mesh-sync/determinism concerns that
// apply to the shared bars/matrix patterns are relevant here: plain
// rand() throughout is fine.

#include "patterns/shared.h"
#include <led_viz_esp32.h>
#include <stdlib.h>

#define WA_NUM_STRIPS 2
static const int wa_strip_ids[WA_NUM_STRIPS] = {5, 6};
#define WA_MAX_LEDS 64

typedef enum { WA_MODE_STAR, WA_MODE_SPARKLE, WA_MODE_PULSE, WA_MODE_COUNT } WaMode;

#define WA_MODE_DURATION_MIN_MS 15000.0
#define WA_MODE_DURATION_MAX_MS 30000.0

// Shooting star
#define WA_STAR_TRAVEL_MS 1200.0   // time to cross the whole strip
#define WA_STAR_GAP_MIN_MS 1500.0  // pause between stars
#define WA_STAR_GAP_MAX_MS 4000.0
#define WA_STAR_FADE_PER_FRAME 6.0f // afterglow decay speed (0-255 scale) -- slow, for a long, strong trail
// Chance (out of 100) a given star sweeps the full color wheel by
// position (PALETTE_RAINBOW) instead of one fixed palette color.
#define WA_STAR_RAINBOW_CHANCE_PERCENT 25

// Sparkle -- a rare, self-contained burst: starts near-silent, ramps up
// to a lot of white twinkles, then fades back out over the mode's
// duration, rather than sparkling at a constant rate the whole time.
#define WA_SPARKLE_CHANCE_PER_LED_PEAK 25 // out of 1000, chance per LED per frame at the peak of the burst
#define WA_SPARKLE_FADE_PER_FRAME 18.0f
// Envelope shaping: raising the sine envelope to this power flattens it
// out near both ends (a slow start from near-nothing and a slow fade at
// the end) while keeping the same overall burst duration -- a plain
// sin() rises fastest right at the start, which reads as too sudden.
#define WA_SPARKLE_ENVELOPE_POWER 3.0

// Pulse
#define WA_PULSE_PERIOD_MS 4000.0
#define WA_PULSE_MIN 20
#define WA_PULSE_MAX 90
#define WA_PULSE_TWO_PI 6.28318530717958647692

#define WA_PI 3.14159265358979323846

static WaMode wa_mode = WA_MODE_PULSE;
static double wa_mode_start_ms = 0.0;
static double wa_mode_duration_ms = 0.0;
static double wa_next_mode_change_ms = 0.0;
static bool wa_started = false;

typedef struct {
    float fb_r[WA_MAX_LEDS], fb_g[WA_MAX_LEDS], fb_b[WA_MAX_LEDS]; // persistent, faded each frame

    // shooting star -- meaning of star_event_ms depends on star_active:
    // the time the current star launched (active), or the time the next
    // one should launch (not active).
    bool star_active;
    double star_event_ms;
    RGB star_color;   // used unless star_rainbow
    bool star_rainbow; // this star sweeps the full color wheel by
                       // position instead of one fixed palette color

    // pulse
    RGB pulse_color;
    bool pulse_color_picked;
} WaStrip;

static WaStrip wa_state[WA_NUM_STRIPS];

static void wa_reset_strip(WaStrip *s) {
    for (int i = 0; i < WA_MAX_LEDS; i++) {
        s->fb_r[i] = 0.0f;
        s->fb_g[i] = 0.0f;
        s->fb_b[i] = 0.0f;
    }
    s->star_active = false;
    s->star_event_ms = 0.0;
    s->star_rainbow = false;
    s->pulse_color_picked = false;
}

static double wa_mode_duration(void) {
    double span = WA_MODE_DURATION_MAX_MS - WA_MODE_DURATION_MIN_MS;
    return WA_MODE_DURATION_MIN_MS + (double)(rand() % (int)span);
}

static WaMode wa_pick_mode(WaMode avoid) {
    WaMode next = (WaMode)(rand() % WA_MODE_COUNT);
    if (next == avoid) next = (WaMode)((next + 1) % WA_MODE_COUNT);
    return next;
}

static void wa_fade(WaStrip *s, int n, float amount) {
    for (int i = 0; i < n; i++) {
        s->fb_r[i] = (s->fb_r[i] > amount) ? s->fb_r[i] - amount : 0.0f;
        s->fb_g[i] = (s->fb_g[i] > amount) ? s->fb_g[i] - amount : 0.0f;
        s->fb_b[i] = (s->fb_b[i] > amount) ? s->fb_b[i] - amount : 0.0f;
    }
}

static void wa_update_star(WaStrip *s, double time_ms, int n,
                          const Palette16 palette) {
    wa_fade(s, n, WA_STAR_FADE_PER_FRAME);

    if (!s->star_active) {
        if (time_ms >= s->star_event_ms) {
            s->star_active = true;
            s->star_event_ms = time_ms; // now means "launch time"
            s->star_rainbow = (rand() % 100) < WA_STAR_RAINBOW_CHANCE_PERCENT;
            if (!s->star_rainbow) {
                uint8_t hue = (uint8_t)(rand() & 0xFF);
                s->star_color = palette_sample(palette, hue, 255, true);
            }
        }
        return;
    }

    double t = (time_ms - s->star_event_ms) / WA_STAR_TRAVEL_MS; // 0..1
    if (t >= 1.0) {
        s->star_active = false;
        double gap = WA_STAR_GAP_MIN_MS +
                     (double)(rand() % (int)(WA_STAR_GAP_MAX_MS - WA_STAR_GAP_MIN_MS));
        s->star_event_ms = time_ms + gap; // now means "next launch time"
        return;
    }

    int pos = (int)(t * (n - 1) + 0.5);
    if (pos >= 0 && pos < n) {
        RGB c;
        if (s->star_rainbow) {
            // Hue tied to position (not time), so the trail left behind
            // as it travels reads as a rainbow gradient across the strip
            // rather than a single color shifting over time.
            uint8_t hue = (uint8_t)((pos * 255) / (n > 1 ? (n - 1) : 1));
            c = palette_sample(PALETTE_RAINBOW, hue, 255, true);
        } else {
            c = s->star_color;
        }
        s->fb_r[pos] = c.r;
        s->fb_g[pos] = c.g;
        s->fb_b[pos] = c.b;
    }
}

static void wa_update_sparkle(WaStrip *s, double time_ms, int n) {
    wa_fade(s, n, WA_SPARKLE_FADE_PER_FRAME);

    // Envelope over the mode's duration: 0 at the start, peaks at the
    // midpoint, back to 0 at the end -- one rare burst of activity
    // instead of a constant sparkle rate for the whole mode.
    double phase = (wa_mode_duration_ms > 0.0)
                       ? (time_ms - wa_mode_start_ms) / wa_mode_duration_ms
                       : 0.0;
    if (phase < 0.0) phase = 0.0;
    if (phase > 1.0) phase = 1.0;
    double envelope = pow(sin(phase * WA_PI), WA_SPARKLE_ENVELOPE_POWER);
    int chance = (int)(envelope * WA_SPARKLE_CHANCE_PER_LED_PEAK);
    if (chance <= 0) return;

    for (int i = 0; i < n; i++) {
        if ((rand() % 1000) < chance) {
            s->fb_r[i] = 255.0f;
            s->fb_g[i] = 255.0f;
            s->fb_b[i] = 255.0f;
        }
    }
}

static void wa_update_pulse(WaStrip *s, double time_ms, int n,
                           const Palette16 palette) {
    if (!s->pulse_color_picked) {
        uint8_t hue = (uint8_t)(rand() & 0xFF);
        s->pulse_color = palette_sample(palette, hue, 255, true);
        s->pulse_color_picked = true;
    }

    double phase = fmod(time_ms, WA_PULSE_PERIOD_MS) / WA_PULSE_PERIOD_MS;
    double eased = (sin(phase * WA_PULSE_TWO_PI - 1.5707963267949) + 1.0) * 0.5; // 0..1
    uint8_t level = (uint8_t)(WA_PULSE_MIN + eased * (WA_PULSE_MAX - WA_PULSE_MIN));

    for (int i = 0; i < n; i++) {
        s->fb_r[i] = scale8(s->pulse_color.r, level);
        s->fb_g[i] = scale8(s->pulse_color.g, level);
        s->fb_b[i] = scale8(s->pulse_color.b, level);
    }
}

// OverlayFunc-compatible: replaces whatever the active pattern drew on
// strips 5/6 with the ambient effect. Leaves every other strip untouched.
void ws2801_ambient_overlay(double time_ms, PixelFunc pixel,
                            GetPixelFunc get_pixel, const Palette16 palette) {
    (void)get_pixel;

    if (!wa_started) {
        wa_mode = wa_pick_mode((WaMode)-1);
        wa_mode_start_ms = time_ms;
        wa_mode_duration_ms = wa_mode_duration();
        wa_next_mode_change_ms = time_ms + wa_mode_duration_ms;
        for (int k = 0; k < WA_NUM_STRIPS; k++) wa_reset_strip(&wa_state[k]);
        wa_started = true;
    }

    if (time_ms >= wa_next_mode_change_ms) {
        wa_mode = wa_pick_mode(wa_mode);
        wa_mode_start_ms = time_ms;
        wa_mode_duration_ms = wa_mode_duration();
        wa_next_mode_change_ms = time_ms + wa_mode_duration_ms;
        for (int k = 0; k < WA_NUM_STRIPS; k++) wa_reset_strip(&wa_state[k]);
    }

    for (int k = 0; k < WA_NUM_STRIPS; k++) {
        int strip_id = wa_strip_ids[k];
        int n = get_strip_num_leds(strip_id);
        if (n <= 0) continue;
        if (n > WA_MAX_LEDS) n = WA_MAX_LEDS;

        WaStrip *s = &wa_state[k];
        switch (wa_mode) {
            case WA_MODE_STAR:    wa_update_star(s, time_ms, n, palette); break;
            case WA_MODE_SPARKLE: wa_update_sparkle(s, time_ms, n); break;
            case WA_MODE_PULSE:   wa_update_pulse(s, time_ms, n, palette); break;
            default: break;
        }

        for (int i = 0; i < n; i++) {
            uint8_t r = (uint8_t)(s->fb_r[i] + 0.5f);
            uint8_t g = (uint8_t)(s->fb_g[i] + 0.5f);
            uint8_t b = (uint8_t)(s->fb_b[i] + 0.5f);
            pixel(strip_id, i, &r, &g, &b);
        }
    }
}
