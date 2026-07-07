// Sine Loop pattern -- ported from schlitzerei/src/patterns/SineLoop.cpp
//
// Diverges from the original in a few ways, all intentional:
//   - the original samples raw CHSV hues directly; this ports "hue" onto
//     the currently active Palette16 as a rotating index instead, matching
//     every other pattern in this codebase (Noise, Gradient Sweep, ...) so
//     Sine respects whatever palette is selected rather than a fixed
//     hardcoded hue wheel.
//   - the original's brightness went through
//     `map(sin8(...), 0, 1, 0, 255)` -- an Arduino map() call with a
//     from-range of [0,1], which (given sin8 already returns 0-255) reads
//     like a copy-paste typo rather than an intentional effect; the raw
//     sin8 output is used as brightness directly instead.
//   - per-frame time step is driven by elapsed real time rather than a
//     fixed divisor of a per-call counter, matching noise.c/searchlight.c's
//     frame-rate-independence rationale.
//   - the two sine layers are additively blended (matching the original's
//     `leds[i] += ...` onto a persistent, per-frame-faded framebuffer --
//     Patterns.cpp's global decay_rate=100 while Sine is active), then
//     desaturated (the shared/common channel subtracted back out) so their
//     overlap reads as color instead of washing out toward white.

#include "shared.h"
#include <led_viz.h>

#define SINE_FB_MAX_LEDS 256
static uint8_t sine_fb_r[7][SINE_FB_MAX_LEDS];
static uint8_t sine_fb_g[7][SINE_FB_MAX_LEDS];
static uint8_t sine_fb_b[7][SINE_FB_MAX_LEDS];

// Matches the original's global decay_rate=100 applied every frame while
// Sine is active: FastLED's fadeToBlackBy(100) scales each channel via
// nscale8(255-100)=155.
#define SINE_FADE_SCALE 155

// Two independently-moving sine layers -- matches the original's
// speed_1/frequency_1 (layer "1") and speed_2/frequency_2 (layer "2");
// a third layer was commented out in the original and is skipped here too.
#define SINE_SPEED_1 9
#define SINE_FREQUENCY_1 2
#define SINE_HUE_1_BASE 32
#define SINE_HUE_1_SWING 100.0

#define SINE_SPEED_2 5
#define SINE_FREQUENCY_2 5
#define SINE_HUE_2_BASE 128
#define SINE_HUE_2_SWING 100.0

// Approximates FastLED's sin8: a full sine cycle mapped to 0-255 in/out.
static uint8_t sine_sin8(uint8_t theta) {
    float rad = (float)theta * (6.28318530f / 256.0f);
    return (uint8_t)(128.0f + 127.0f * sinf(rad) + 0.5f);
}

// Subtracts the shared (achromatic) component from an RGB pixel -- additive
// blending of two differently-hued layers onto the same still-fading pixel
// otherwise pushes all three channels up together and reads as white rather
// than a blend of colors.
static void sine_desaturate(uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t m = *r;
    if (*g < m) m = *g;
    if (*b < m) m = *b;
    *r = (uint8_t)(*r - m);
    *g = (uint8_t)(*g - m);
    *b = (uint8_t)(*b - m);
}

static void sine_fade_all(void) {
    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            sine_fb_r[s][i] = scale8(sine_fb_r[s][i], SINE_FADE_SCALE);
            sine_fb_g[s][i] = scale8(sine_fb_g[s][i], SINE_FADE_SCALE);
            sine_fb_b[s][i] = scale8(sine_fb_b[s][i], SINE_FADE_SCALE);
        }
    }
}

// factor: per-strip spatial-frequency multiplier -- matches the original's
// per-strip call (bar_1=1, bar_2=2, bar_3=3, bar_4=4, matrix=3, strip_1=1,
// strip_2=1), giving each strip a visually distinct wave density. The
// matrix is treated as one flat run (like the original's flat CRGB array),
// not decomposed into rows/columns -- Sine never used XY() in the original.
static void sine_render_strip(int strip_id, int factor, uint8_t t1, uint8_t t2,
                              uint8_t idx1, uint8_t idx2,
                              const Palette16 palette) {
    int n = get_strip_num_leds(strip_id);
    if (n <= 0) return;
    if (n > SINE_FB_MAX_LEDS) n = SINE_FB_MAX_LEDS;

    for (int i = 0; i < n; i++) {
        uint8_t theta1 = (uint8_t)(i * SINE_FREQUENCY_1 * factor + t1);
        uint8_t theta2 = (uint8_t)(i * SINE_FREQUENCY_2 * factor + t2);

        uint8_t val1 = sine_sin8(theta1);
        uint8_t val2 = sine_sin8(theta2);

        RGB c1 = palette_sample(palette, idx1, val1, true);
        RGB c2 = palette_sample(palette, idx2, val2, true);

        sine_fb_r[strip_id][i] = qadd8(qadd8(sine_fb_r[strip_id][i], c1.r), c2.r);
        sine_fb_g[strip_id][i] = qadd8(qadd8(sine_fb_g[strip_id][i], c1.g), c2.g);
        sine_fb_b[strip_id][i] = qadd8(qadd8(sine_fb_b[strip_id][i], c1.b), c2.b);
        sine_desaturate(&sine_fb_r[strip_id][i], &sine_fb_g[strip_id][i],
                        &sine_fb_b[strip_id][i]);
    }
}

static void sine_update(double time_ms, PixelFunc pixel,
                        const Palette16 palette) {
    double mod1 = noise_mod1(time_ms);
    double mod2 = noise_mod2(time_ms);

    uint8_t idx1 = (uint8_t)(SINE_HUE_1_BASE + (int)(mod1 * SINE_HUE_1_SWING));
    uint8_t idx2 = (uint8_t)(SINE_HUE_2_BASE + (int)(mod2 * SINE_HUE_2_SWING));

    // Only the low 8 bits ever matter (sin8's argument is a byte), so a
    // plain truncating cast reproduces the original's implicit narrowing
    // when `now / speed` was passed into sin8() -- correct even after long
    // uptimes once time_ms grows past a uint8_t's range.
    uint32_t t_fast = (uint32_t)time_ms;
    uint8_t t1 = (uint8_t)(t_fast / SINE_SPEED_1);
    uint8_t t2 = (uint8_t)(t_fast / SINE_SPEED_2);

    sine_fade_all();

    sine_render_strip(0, 1, t1, t2, idx1, idx2, palette); // bar_1
    sine_render_strip(1, 2, t1, t2, idx1, idx2, palette); // bar_2
    sine_render_strip(2, 3, t1, t2, idx1, idx2, palette); // bar_3
    sine_render_strip(3, 4, t1, t2, idx1, idx2, palette); // bar_4
    sine_render_strip(4, 3, t1, t2, idx1, idx2, palette); // matrix (flat)
    sine_render_strip(5, 1, t1, t2, idx1, idx2, palette); // strip_1
    sine_render_strip(6, 1, t1, t2, idx1, idx2, palette); // strip_2

    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        if (n > SINE_FB_MAX_LEDS) n = SINE_FB_MAX_LEDS;
        for (int i = 0; i < n; i++) {
            pixel(s, i, &sine_fb_r[s][i], &sine_fb_g[s][i], &sine_fb_b[s][i]);
        }
    }
}
