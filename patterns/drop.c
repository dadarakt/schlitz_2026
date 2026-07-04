// Drop pattern -- ported from schlitzerei/src/patterns/DropPattern.cpp
//
// Spawns soft falloff "blobs" at random positions on bars/matrix/strips at
// tunable rates, left to fade via drop_fade_all(). Renders into its own
// persistent frame buffer (see noise.c) since PixelFunc is write-then-read,
// not suited to blending across multiple draw calls.

#include "shared.h"
#include <led_viz.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Spawn rates (events per second, combined across the group)
#define DROP_BASE_BARS_PER_SEC   2.5f
#define DROP_BASE_STRIPS_PER_SEC 1.5f
#define DROP_BASE_MATRIX_PER_SEC 6.0f

// mod1/mod2 modulate the rates: norm(mod) in [0,1] -> rate *= lerp(min,max)
#define DROP_MOD_MIN_MUL 0.5f
#define DROP_MOD_MAX_MUL 2.0f

// Bars get bigger, softer blobs than strips/matrix
#define DROP_BAR_RADIUS_MUL 4.0f
#define DROP_BAR_RADIUS_CAP 80

#define DROP_MIN_R1D 3
#define DROP_MAX_R1D 12
#define DROP_MIN_R2D 3
#define DROP_MAX_R2D 8

#define DROP_PALETTE_BRIGHTNESS 230 // headroom for other overlays

// Per-frame fade -- Patterns.cpp sets decay_rate=20 while Drop is active.
// FastLED's fadeToBlackBy(20) scales via nscale8(255-20)=235.
#define DROP_FADE_SCALE 235

#define DROP_FB_MAX_LEDS 256
static uint8_t drop_fb_r[7][DROP_FB_MAX_LEDS];
static uint8_t drop_fb_g[7][DROP_FB_MAX_LEDS];
static uint8_t drop_fb_b[7][DROP_FB_MAX_LEDS];

static double drop_last_time_ms = -1.0;
static float drop_acc_bars = 0.0f;
static float drop_acc_strips = 0.0f;
static float drop_acc_matrix = 0.0f;

static int drop_rand_range(int lo, int hi_exclusive) {
    if (hi_exclusive <= lo) return lo;
    return lo + (rand() % (hi_exclusive - lo));
}

static float drop_clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static float drop_norm_mod(double m) { return drop_clamp01((float)((m + 1.0) * 0.5)); }
static float drop_lerp(float a, float b, float t) { return a + (b - a) * t; }

static void drop_add_scaled(int strip_id, int led, RGB color, uint8_t scale) {
    if (!scale) return;
    drop_fb_r[strip_id][led] = qadd8(drop_fb_r[strip_id][led], scale8(color.r, scale));
    drop_fb_g[strip_id][led] = qadd8(drop_fb_g[strip_id][led], scale8(color.g, scale));
    drop_fb_b[strip_id][led] = qadd8(drop_fb_b[strip_id][led], scale8(color.b, scale));
}

// Soft 1D blob: full brightness at center, tapering to 0 at +-radius.
static void drop_blob_1d(int strip_id, int count, int center, uint8_t radius,
                         RGB color) {
    int start = center - (int)radius;
    int end = center + (int)radius;
    if (start < 0) start = 0;
    if (end > count - 1) end = count - 1;

    for (int i = start; i <= end; i++) {
        int d = abs(i - center);
        uint8_t w = (radius == 0) ? 255 : (uint8_t)(255 * (radius - d) / radius);
        w = dim8_raw(w); // soften the falloff curve
        drop_add_scaled(strip_id, i, color, w);
    }
}

// Soft 2D blob on the matrix.
static void drop_blob_2d(int strip_id, int width, int height, uint8_t cx,
                         uint8_t cy, uint8_t radius, RGB color) {
    int xmin = cx - radius, xmax = cx + radius;
    int ymin = cy - radius, ymax = cy + radius;
    if (xmin < 0) xmin = 0;
    if (xmax > width - 1) xmax = width - 1;
    if (ymin < 0) ymin = 0;
    if (ymax > height - 1) ymax = height - 1;

    int r2 = (int)radius * (int)radius;
    for (int x = xmin; x <= xmax; x++) {
        for (int y = ymin; y <= ymax; y++) {
            int dx = x - cx, dy = y - cy;
            int dist2 = dx * dx + dy * dy;
            if (dist2 > r2) continue;
            float d = sqrtf((float)dist2);
            float lin = 1.0f - d / (float)radius;
            if (lin < 0) lin = 0;
            uint8_t w = (uint8_t)(lin * 255.0f + 0.5f);
            w = dim8_raw(w);
            int led = get_matrix_index(strip_id, x, y);
            drop_add_scaled(strip_id, led, color, w);
        }
    }
}

static RGB drop_random_color(const Palette16 palette) {
    return palette_sample(palette, (uint8_t)(rand() & 0xFF),
                          DROP_PALETTE_BRIGHTNESS, true);
}

static void drop_spawn_bar(const Palette16 palette) {
    int bar = rand() % 4; // strip indices 0-3
    int n = get_strip_num_leds(bar);
    if (n <= 0) return;

    int center = rand() % n;
    int base = drop_rand_range(DROP_MIN_R1D, DROP_MAX_R1D + 1);
    int scaled = (int)(base * DROP_BAR_RADIUS_MUL + 0.5f);
    if (scaled < DROP_MIN_R1D) scaled = DROP_MIN_R1D;
    if (scaled > DROP_BAR_RADIUS_CAP) scaled = DROP_BAR_RADIUS_CAP;

    drop_blob_1d(bar, n, center, (uint8_t)scaled, drop_random_color(palette));
}

static void drop_spawn_strip(const Palette16 palette) {
    int strip_id = (rand() & 1) ? 6 : 5; // WS2801 strips
    int n = get_strip_num_leds(strip_id);
    if (n <= 0) return;

    int center = rand() % n;
    int radius = drop_rand_range(DROP_MIN_R1D, DROP_MAX_R1D + 1);
    drop_blob_1d(strip_id, n, center, (uint8_t)radius, drop_random_color(palette));
}

static void drop_spawn_matrix(const Palette16 palette) {
    int width = get_matrix_width(4);
    int height = get_matrix_height(4);
    if (width <= 0 || height <= 0) return;

    int cx = rand() % width;
    int cy = rand() % height;
    int radius = drop_rand_range(DROP_MIN_R2D, DROP_MAX_R2D + 1);
    drop_blob_2d(4, width, height, (uint8_t)cx, (uint8_t)cy, (uint8_t)radius,
                drop_random_color(palette));
}

static void drop_fade_all(void) {
    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            drop_fb_r[s][i] = scale8(drop_fb_r[s][i], DROP_FADE_SCALE);
            drop_fb_g[s][i] = scale8(drop_fb_g[s][i], DROP_FADE_SCALE);
            drop_fb_b[s][i] = scale8(drop_fb_b[s][i], DROP_FADE_SCALE);
        }
    }
}

static void drop_init(void) {
    memset(drop_fb_r, 0, sizeof(drop_fb_r));
    memset(drop_fb_g, 0, sizeof(drop_fb_g));
    memset(drop_fb_b, 0, sizeof(drop_fb_b));
    drop_last_time_ms = -1.0;
    drop_acc_bars = drop_acc_strips = drop_acc_matrix = 0.0f;
}

static void drop_update(double time_ms, PixelFunc pixel,
                        const Palette16 palette) {
    double dt_ms = (drop_last_time_ms < 0.0) ? 16.0 : (time_ms - drop_last_time_ms);
    drop_last_time_ms = time_ms;
    if (dt_ms > 80.0) dt_ms = 80.0; // clamp pathological gaps (e.g. after a pause)
    float seconds = (float)(dt_ms / 1000.0);

    double mod1 = noise_mod1(time_ms);
    double mod2 = noise_mod2(time_ms);
    float mul_bars   = drop_lerp(DROP_MOD_MIN_MUL, DROP_MOD_MAX_MUL, drop_norm_mod(mod1));
    float mul_strips = drop_lerp(DROP_MOD_MIN_MUL, DROP_MOD_MAX_MUL, drop_norm_mod(mod1));
    float mul_matrix = drop_lerp(DROP_MOD_MIN_MUL, DROP_MOD_MAX_MUL, drop_norm_mod(mod2));

    drop_fade_all();

    drop_acc_bars   += DROP_BASE_BARS_PER_SEC   * mul_bars   * seconds;
    drop_acc_strips += DROP_BASE_STRIPS_PER_SEC * mul_strips * seconds;
    drop_acc_matrix += DROP_BASE_MATRIX_PER_SEC * mul_matrix * seconds;

    while (drop_acc_bars >= 1.0f)   { drop_spawn_bar(palette);    drop_acc_bars   -= 1.0f; }
    while (drop_acc_strips >= 1.0f) { drop_spawn_strip(palette);  drop_acc_strips -= 1.0f; }
    while (drop_acc_matrix >= 1.0f) { drop_spawn_matrix(palette); drop_acc_matrix -= 1.0f; }

    // Fractional leftover chance for one extra spawn per group, so low rates
    // still feel alive instead of only firing on whole-event boundaries.
    if (drop_acc_bars > 0.0f && (rand() & 0xFF) < (uint8_t)(drop_acc_bars * 255.0f)) {
        drop_spawn_bar(palette);
        drop_acc_bars = 0.0f;
    }
    if (drop_acc_strips > 0.0f && (rand() & 0xFF) < (uint8_t)(drop_acc_strips * 255.0f)) {
        drop_spawn_strip(palette);
        drop_acc_strips = 0.0f;
    }
    if (drop_acc_matrix > 0.0f && (rand() & 0xFF) < (uint8_t)(drop_acc_matrix * 255.0f)) {
        drop_spawn_matrix(palette);
        drop_acc_matrix = 0.0f;
    }

    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            pixel(s, i, &drop_fb_r[s][i], &drop_fb_g[s][i], &drop_fb_b[s][i]);
        }
    }
}
