// Wave pattern -- ported from schlitzerei/src/patterns/WaveRenderer.cpp
//
// A wave front expands through three phases over its lifetime: the matrix
// (first 20%), then all 4 bars (next 40%), then the WS2801 strips (last
// 40%). Renders into its own persistent frame buffer (see noise.c) since
// PixelFunc is write-then-read, not suited to blending across draw calls.

#include "shared.h"
#include <led_viz.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WAVE_MAX_WAVES 50

// Per-frame fade. The original literally uses fadeToBlackBy(5) here
// (nscale8(255-5)=250, ~98% retained/frame) -- deliberately NOT matched:
// with up to WAVE_MAX_WAVES full-brightness waves able to overlap, and each
// LED getting hit by a new wave roughly every ~125ms on average (spawn
// interval 100-500ms, ~8 concurrent waves typical) but that gentle a decay's
// time constant is closer to ~800ms, additions land faster than they clear
// and every channel ratchets up to 255 and sticks there -- reads as "no
// decay" even though it's technically still happening, just far too slowly
// relative to the addition rate. Retune this by eye against hardware.
#define WAVE_FADE_SCALE 205

typedef struct {
    double start_time_ms;
    double duration_ms;
    RGB color;
    float feather_width;
} Wave;

static Wave wave_list[WAVE_MAX_WAVES];
static int wave_count = 0;
static double wave_next_spawn_ms = 0.0;

#define WAVE_FB_MAX_LEDS 256
static uint8_t wave_fb_r[7][WAVE_FB_MAX_LEDS];
static uint8_t wave_fb_g[7][WAVE_FB_MAX_LEDS];
static uint8_t wave_fb_b[7][WAVE_FB_MAX_LEDS];

static int wave_rand_range(int lo, int hi_exclusive) {
    if (hi_exclusive <= lo) return lo;
    return lo + (rand() % (hi_exclusive - lo));
}

static void wave_add_scaled(int strip_id, int led, RGB color, uint8_t weight) {
    if (!weight) return;
    wave_fb_r[strip_id][led] = qadd8(wave_fb_r[strip_id][led], scale8(color.r, weight));
    wave_fb_g[strip_id][led] = qadd8(wave_fb_g[strip_id][led], scale8(color.g, weight));
    wave_fb_b[strip_id][led] = qadd8(wave_fb_b[strip_id][led], scale8(color.b, weight));
}

// Linear tent falloff around `radius`, +-feather wide.
static uint8_t wave_band_weight(float distance, float radius, float feather) {
    float w = 1.0f - fabsf(distance - radius) / feather;
    if (w < 0.0f) w = 0.0f;
    return (uint8_t)(w * 255.0f + 0.5f);
}

static void wave_spawn(double time_ms, const Palette16 palette) {
    if (wave_count >= WAVE_MAX_WAVES) return;

    Wave *w = &wave_list[wave_count++];
    w->start_time_ms = time_ms;
    w->color = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
    w->feather_width = (float)wave_rand_range(3, 20);
    w->duration_ms = (double)wave_rand_range(1000, 4000);

    wave_next_spawn_ms = time_ms + wave_rand_range(100, 500);
}

static void wave_fade_all(void) {
    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            wave_fb_r[s][i] = scale8(wave_fb_r[s][i], WAVE_FADE_SCALE);
            wave_fb_g[s][i] = scale8(wave_fb_g[s][i], WAVE_FADE_SCALE);
            wave_fb_b[s][i] = scale8(wave_fb_b[s][i], WAVE_FADE_SCALE);
        }
    }
}

static void wave_render_one(const Wave *w, double age) {
    double matrix_phase = w->duration_ms * 0.2;
    double bar_phase = w->duration_ms * 0.4;
    double strip_phase = w->duration_ms * 0.4;

    if (age < matrix_phase) {
        int width = get_matrix_width(4);
        int height = get_matrix_height(4);
        if (width <= 0 || height <= 0) return;

        float progress = (float)(age / matrix_phase);
        float radius = (width / 2.0f) * progress;
        int center = width / 2;

        for (int x = 0; x < width; x++) {
            float distance = fabsf((float)(x - center));
            uint8_t weight = wave_band_weight(distance, radius, w->feather_width);
            if (!weight) continue;
            for (int y = 0; y < height; y++) {
                int led = get_matrix_index(4, x, y);
                wave_add_scaled(4, led, w->color, weight);
            }
        }
    } else if (age < matrix_phase + bar_phase) {
        float progress = (float)((age - matrix_phase) / bar_phase);

        for (int bar = 0; bar < 4; bar++) {
            int n = get_strip_num_leds(bar);
            float radius = n * progress;
            for (int i = 0; i < n; i++) {
                uint8_t weight = wave_band_weight((float)i, radius, w->feather_width);
                if (!weight) continue;
                wave_add_scaled(bar, i, w->color, weight);
            }
        }
    } else {
        float progress = (float)((age - matrix_phase - bar_phase) / strip_phase);

        for (int s = 5; s <= 6; s++) {
            int n = get_strip_num_leds(s);
            float radius = n * progress;
            for (int i = 0; i < n; i++) {
                uint8_t weight = wave_band_weight((float)i, radius, w->feather_width);
                if (!weight) continue;
                wave_add_scaled(s, i, w->color, weight);
            }
        }
    }
}

static void wave_init(void) {
    memset(wave_fb_r, 0, sizeof(wave_fb_r));
    memset(wave_fb_g, 0, sizeof(wave_fb_g));
    memset(wave_fb_b, 0, sizeof(wave_fb_b));
    wave_count = 0;
    wave_next_spawn_ms = 0.0;
}

static void wave_update(double time_ms, PixelFunc pixel,
                        const Palette16 palette) {
    wave_fade_all();

    if (time_ms > wave_next_spawn_ms && wave_count < WAVE_MAX_WAVES) {
        wave_spawn(time_ms, palette);
    }

    // Render + expire waves, compacting the array (order doesn't matter --
    // blending is commutative, so swap-with-last is fine unlike a std::vector
    // erase that must preserve order).
    for (int i = 0; i < wave_count; ) {
        double age = time_ms - wave_list[i].start_time_ms;
        if (age >= wave_list[i].duration_ms) {
            wave_list[i] = wave_list[wave_count - 1];
            wave_count--;
            continue;
        }
        wave_render_one(&wave_list[i], age);
        i++;
    }

    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            pixel(s, i, &wave_fb_r[s][i], &wave_fb_g[s][i], &wave_fb_b[s][i]);
        }
    }
}
