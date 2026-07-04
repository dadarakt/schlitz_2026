// Searchlight pattern -- ported from schlitzerei/src/patterns/SearchlightPattern.cpp,
// with the sweep redesigned around this installation's actual layout:
// two bars, then the matrix, then two more bars (bar1 bar2 | matrix | bar3
// bar4). The sweep's beam re-weights this as 5 *equal* stops rather than
// raw LED/column counts, so each bar and the matrix get even "screen time"
// as the beam crosses -- the original's raw-column weighting let the
// matrix's 32 columns dominate a bar's single pixel, which read as the
// beam crawling slowly across the matrix and barely visiting the bars.
//
// Two layers: a constantly-running palette-driven "glimmer" texture on
// every strip (fully overwritten each frame -- no persistent fade needed,
// unlike noise/drop/wave), with an intermittent white searchlight beam
// swept additively on top, active for random 10-20s stretches separated by
// random 10-20s gaps. Within an active stretch, the beam swings side to
// side between bar1 and bar4 with a cosine ease (naturally slower near each
// side, fastest through the matrix in the middle), each one-way swing
// randomized so round trips land around 3-5s without repeating a fixed
// period.

#include "shared.h"
#include <led_viz.h>
#include <math.h>
#include <stdlib.h>

#define SL_FB_MAX_LEDS 256
static uint8_t sl_fb_r[7][SL_FB_MAX_LEDS];
static uint8_t sl_fb_g[7][SL_FB_MAX_LEDS];
static uint8_t sl_fb_b[7][SL_FB_MAX_LEDS];

static bool sl_active = false;
static double sl_active_start_ms = 0.0;
static double sl_active_duration_ms = 0.0;
static double sl_next_activation_ms = 0.0;

static int sl_rand_range(int lo, int hi_exclusive) {
    if (hi_exclusive <= lo) return lo;
    return lo + (rand() % (hi_exclusive - lo));
}

// Approximates FastLED's sin8: a full sine cycle mapped to 0-255 in/out.
static uint8_t sl_sin8(uint8_t theta) {
    float rad = (float)theta * (6.28318530f / 256.0f);
    return (uint8_t)(128.0f + 127.0f * sinf(rad) + 0.5f);
}

// Approximates FastLED's sqrt16: integer sqrt of a 0-65535 value into 0-255.
static uint8_t sl_sqrt16(uint16_t x) { return (uint8_t)sqrtf((float)x); }

static void sl_set(int strip_id, int led, RGB color) {
    sl_fb_r[strip_id][led] = color.r;
    sl_fb_g[strip_id][led] = color.g;
    sl_fb_b[strip_id][led] = color.b;
}

static void sl_add_white(int strip_id, int led, uint8_t v) {
    if (!v) return;
    sl_fb_r[strip_id][led] = qadd8(sl_fb_r[strip_id][led], v);
    sl_fb_g[strip_id][led] = qadd8(sl_fb_g[strip_id][led], v);
    sl_fb_b[strip_id][led] = qadd8(sl_fb_b[strip_id][led], v);
}

// Palette-driven glimmer texture for a 1D strip (bar or WS2801 strip).
static void sl_glimmer_1d(int strip_id, int offset, double time_ms,
                          double mod1, const Palette16 palette) {
    int n = get_strip_num_leds(strip_id);
    if (n <= 0) return;

    uint32_t t_fast = (uint32_t)time_ms;
    // Two casts (double -> uint32_t -> uint16_t), not a direct cast to
    // uint16_t: the factor is always < 1, so the product stays bounded by
    // t_fast (safely representable/convertible as uint32_t) before the
    // final well-defined narrowing truncation -- avoids UB from an
    // out-of-range float-to-uint16_t conversion on long uptimes.
    uint16_t t_noise = (uint16_t)(uint32_t)((double)t_fast *
        (0.020 + 0.020 * ((mod1 + 1.0) * 0.5)));
    uint8_t scroll = (uint8_t)(t_fast >> 5);

    for (int i = 0; i < n; i++) {
        uint16_t x = (uint16_t)(i * 13 + offset);

        // Original calls the 2D inoise8(x, tNoise) overload; our SDK only
        // has the 3D form, so we fix the 3rd axis at 0 -- still smooth
        // Perlin noise, just not bit-identical to FastLED's dedicated 2D path.
        uint8_t n8 = inoise8(x, t_noise, 0);
        uint8_t hf = sl_sin8((uint8_t)(i * 7 + (t_fast >> 2))); // shimmer

        int v16 = (int)n8 + (hf >> 2);
        if (v16 > 255) v16 = 255;

        uint8_t val = sl_sqrt16((uint16_t)(v16 * 257)); // gamma-ish
        val = scale8(val, 220); // cap for sweep-overlay headroom

        // Plain wrapping add (not qadd8): idx is a rotating palette index,
        // not an accumulated brightness -- see noise.c's index/ihue fix.
        uint8_t idx = (uint8_t)(n8 + i * 3 + scroll);

        RGB color = palette_sample(palette, idx, val, true);
        sl_set(strip_id, i, color);
    }
}

// Palette-driven glimmer texture for the matrix.
static void sl_glimmer_matrix(double time_ms, double mod1,
                              const Palette16 palette) {
    int width = get_matrix_width(4);
    int height = get_matrix_height(4);
    if (width <= 0 || height <= 0) return;

    uint32_t t_fast = (uint32_t)time_ms;
    uint16_t t_noise = (uint16_t)(uint32_t)((double)t_fast *
        (0.018 + 0.022 * ((mod1 + 1.0) * 0.5)));
    uint8_t scroll = (uint8_t)(t_fast >> 5);

    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            uint16_t nx = (uint16_t)(x * 17);
            uint16_t ny = (uint16_t)(y * 19);

            uint8_t n8 = inoise8(nx, ny, t_noise);
            uint8_t hf = sl_sin8((uint8_t)(x * 9 + y * 11 + (t_fast >> 2)));

            int v16 = (int)n8 + (hf >> 3); // lighter ripple on matrix
            if (v16 > 255) v16 = 255;

            uint8_t val = sl_sqrt16((uint16_t)(v16 * 257));
            val = scale8(val, 220);

            // Diagonal bias + scroll; wraps like the 1D idx above.
            uint8_t idx = (uint8_t)(n8 + x * 2 - y * 2 + scroll);

            RGB color = palette_sample(palette, idx, val, true);
            int led = get_matrix_index(4, x, y);
            sl_set(4, led, color);
        }
    }
}

static void sl_update_active(double time_ms) {
    if (sl_active) {
        if (time_ms - sl_active_start_ms > sl_active_duration_ms) {
            sl_active = false;
            sl_next_activation_ms = time_ms + sl_rand_range(10000, 20000);
        }
    } else {
        if (time_ms >= sl_next_activation_ms) {
            sl_active = true;
            sl_active_start_ms = time_ms;
            sl_active_duration_ms = sl_rand_range(10000, 20000);
        }
    }
}

// Sweep applied to a bar treated as ONE pixel in the sweep's coordinate
// space (bars are vertical posts -- their whole height lights uniformly).
static void sl_sweep_bar_pixel(int strip_id, float bar_base, float pos,
                               float width) {
    float brightness = 1.0f - fabsf(bar_base - pos) / width;
    if (brightness < 0.0f) brightness = 0.0f;
    uint8_t v = (uint8_t)(brightness * 255.0f + 0.5f);
    if (!v) return;

    int n = get_strip_num_leds(strip_id);
    for (int i = 0; i < n; i++) {
        sl_add_white(strip_id, i, v);
    }
}

// Five equally-weighted stops, left to right: bar1, bar2, matrix, bar3,
// bar4 -- one sweep-space unit apart regardless of each element's actual
// LED/column count, so the beam spends even "time" crossing each one
// instead of the matrix's 32 columns dominating a bar's single pixel.
#define SL_STOP_BAR1   0.0f
#define SL_STOP_BAR2   1.0f
#define SL_STOP_MATRIX 2.0f
#define SL_STOP_BAR3   3.0f
#define SL_STOP_BAR4   4.0f

// Falloff width for the bar stops, in stop-units (1.0 = one full
// stop-spacing). Smaller = a tighter, more focused beam.
#define SL_ZONE_WIDTH 0.7f

// Falloff width for the beam's position *within* the matrix, in columns.
#define SL_MATRIX_BEAM_WIDTH 6.0f

// One-way swing duration range (ms), randomized per swing so a round trip
// (there and back) lands around 3-5s with natural variation rather than a
// fixed metronomic period.
#define SL_SWING_MIN_MS 1500.0
#define SL_SWING_MAX_MS 2500.0

static double sl_swing_start_ms = 0.0;
static double sl_swing_duration_ms = SL_SWING_MIN_MS;
static float sl_swing_from = SL_STOP_BAR1;
static float sl_swing_to = SL_STOP_BAR4;

// Current sweep position, 0 (bar1) .. 4 (bar4), eased with a cosine curve
// so it naturally decelerates approaching each side (like a pendulum) and
// moves fastest through the middle, past the matrix.
static float sl_sweep_position(double time_ms) {
    double t = (time_ms - sl_swing_start_ms) / sl_swing_duration_ms;
    if (t >= 1.0) {
        // Finished this swing: reverse and pick a new randomized duration
        // for the next one, so round trips vary instead of repeating.
        float tmp = sl_swing_from;
        sl_swing_from = sl_swing_to;
        sl_swing_to = tmp;
        sl_swing_duration_ms = sl_rand_range((int)SL_SWING_MIN_MS, (int)SL_SWING_MAX_MS);
        sl_swing_start_ms = time_ms;
        t = 0.0;
    }
    if (t < 0.0) t = 0.0;

    float eased = (float)((1.0 - cos(t * 3.14159265358979323846)) * 0.5); // 0..1
    return sl_swing_from + (sl_swing_to - sl_swing_from) * eased;
}

static void sl_render_sweep(double time_ms) {
    if (!sl_active) return;

    float p = sl_sweep_position(time_ms);

    sl_sweep_bar_pixel(0, SL_STOP_BAR1, p, SL_ZONE_WIDTH);
    sl_sweep_bar_pixel(1, SL_STOP_BAR2, p, SL_ZONE_WIDTH);
    sl_sweep_bar_pixel(2, SL_STOP_BAR3, p, SL_ZONE_WIDTH);
    sl_sweep_bar_pixel(3, SL_STOP_BAR4, p, SL_ZONE_WIDTH);

    // Matrix: map the beam's position relative to the matrix's own stop
    // onto its columns. When the beam is at a bar instead, this naturally
    // extrapolates outside the column range, fading the whole matrix to
    // black via the column-distance falloff below -- no separate "is the
    // beam near the matrix" check needed.
    int mw = get_matrix_width(4);
    int mh = get_matrix_height(4);
    if (mw > 0 && mh > 0) {
        float local = (p - SL_STOP_BAR2) / (SL_STOP_BAR3 - SL_STOP_BAR2); // 0..1 across the matrix's span
        float matrix_col_pos = local * (float)(mw - 1);

        for (int x = 0; x < mw; x++) {
            float brightness = 1.0f - fabsf((float)x - matrix_col_pos) / SL_MATRIX_BEAM_WIDTH;
            if (brightness < 0.0f) brightness = 0.0f;
            uint8_t v = (uint8_t)(brightness * 255.0f + 0.5f);
            if (!v) continue;
            for (int y = 0; y < mh; y++) {
                sl_add_white(4, get_matrix_index(4, x, y), v);
            }
        }
    }
}

static void sl_init(void) {
    sl_active = false;
    sl_active_start_ms = 0.0;
    sl_active_duration_ms = 0.0;
    sl_next_activation_ms = 0.0;

    sl_swing_start_ms = 0.0;
    sl_swing_duration_ms = SL_SWING_MIN_MS;
    sl_swing_from = SL_STOP_BAR1;
    sl_swing_to = SL_STOP_BAR4;
}

static void searchlight_update(double time_ms, PixelFunc pixel,
                               const Palette16 palette) {
    double mod1 = noise_mod1(time_ms);

    sl_update_active(time_ms);

    sl_glimmer_1d(0, 0,    time_ms, mod1, palette);
    sl_glimmer_1d(1, 1000, time_ms, mod1, palette);
    sl_glimmer_1d(2, 2000, time_ms, mod1, palette);
    sl_glimmer_1d(3, 3000, time_ms, mod1, palette);
    sl_glimmer_1d(5, 1000, time_ms, mod1, palette);
    sl_glimmer_1d(6, 0,    time_ms, mod1, palette);
    sl_glimmer_matrix(time_ms, mod1, palette);

    sl_render_sweep(time_ms);

    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            pixel(s, i, &sl_fb_r[s][i], &sl_fb_g[s][i], &sl_fb_b[s][i]);
        }
    }
}
