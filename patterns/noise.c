// Noise pattern -- ported from schlitzerei/src/patterns/NoisePatterns.cpp

#include "shared.h"
#include <led_viz.h>
#include <stdlib.h>

#define NOISE_DIM   32   // MATRIX_MAX_DIMENSION = max(32, 8)

// Per-frame fade amount for the Noise pattern specifically -- Patterns.cpp
// sets decay_rate=50 while Noise is active (not the global default of 100).
// FastLED's fadeToBlackBy(50) scales each channel via nscale8(255-50)=205.
#define FADE_SCALE  205

// Frame buffers – persist across frames to implement fade + additive blending
#define FB_MAX_LEDS 256
static uint8_t fb_r[7][FB_MAX_LEDS];
static uint8_t fb_g[7][FB_MAX_LEDS];
static uint8_t fb_b[7][FB_MAX_LEDS];

// 2D noise field
static uint8_t noise_field[NOISE_DIM][NOISE_DIM];

// Noise walk coordinates (int so they can go negative / wrap naturally)
static int noise_x = 12345;
static int noise_y = 54321;
static int noise_z = 11111;

static void fill_noise(int speed, int nscale) {
    // Blend each new sample with the previous value so the field evolves
    // smoothly frame-to-frame instead of jumping every frame (mirrors
    // NoisePatterns.cpp's fill_noise_8). Note: in the original, data_smoothing
    // is a global initialized once from the *startup* speed value (5), not
    // recomputed per frame despite speed changing -- replicated exactly.
    static const uint8_t data_smoothing = 180; // 200 - (5 * 4)

    for (int i = 0; i < NOISE_DIM; i++) {
        int i_off = nscale * i;
        for (int j = 0; j < NOISE_DIM; j++) {
            int j_off = nscale * j;
            uint8_t data = inoise8((uint16_t)(noise_x + i_off),
                                   (uint16_t)(noise_y + j_off),
                                   (uint16_t)noise_z);

            // Expand the ~16-238 Perlin range toward 0-255
            data = qsub8(data, 16);
            data = qadd8(data, scale8(data, 39));

            // qadd8 (saturating), not plain +: scale8 uses the FastLED-fixed
            // (v*(s+1))>>8 formula, so the two weighted terms can sum to
            // slightly over 255 (e.g. both inputs at 255 -> 180+76 = 256).
            // A plain uint8_t + would wrap 256 -> 0, turning a sustained
            // bright noise peak into a sudden black dropout.
            uint8_t old_data = noise_field[i][j];
            data = qadd8(scale8(old_data, data_smoothing),
                         scale8(data, (uint8_t)(256 - data_smoothing)));

            noise_field[i][j] = data;
        }
    }

    noise_z += speed / 2;
    noise_x += speed / 4;
    noise_y -= speed / 8;
}

// Fade every LED in the frame buffer
static void fb_fade_all(void) {
    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            fb_r[s][i] = scale8(fb_r[s][i], FADE_SCALE);
            fb_g[s][i] = scale8(fb_g[s][i], FADE_SCALE);
            fb_b[s][i] = scale8(fb_b[s][i], FADE_SCALE);
        }
    }
}

// Map the shared noise field onto one strip.
// strip_id    – index into strip_setup[]
// rows, cols  – how many noise rows/columns to use
// offset      – noise coordinate offset (creates visual variety per strip)
// ihue        – rolling hue offset for palette cycling
static void map_noise_to_strip(int strip_id, int rows, int cols, int offset,
                                uint8_t ihue, const Palette16 palette) {
    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            uint8_t index = noise_field[(j + offset) % NOISE_DIM]
                                       [(i + offset) % NOISE_DIM];
            uint8_t bri   = noise_field[abs(i - offset) % NOISE_DIM]
                                       [(j + offset) % NOISE_DIM];

            index = qadd8(index, ihue);  // palette color cycling

            bri = (bri > 127) ? 255 : dim8_raw((uint8_t)(bri * 2));

            RGB color = palette_sample(palette, index, bri, true);

            // Determine the linear LED index for this (i, j) cell
            int led;
            if (rows == 1) {
                led = i;   // 1-D strip: column index is the LED index
            } else {
                led = get_matrix_index(strip_id, i, j);  // 2-D matrix
            }

            // Additive blend into frame buffer (saturating)
            fb_r[strip_id][led] = qadd8(fb_r[strip_id][led], color.r);
            fb_g[strip_id][led] = qadd8(fb_g[strip_id][led], color.g);
            fb_b[strip_id][led] = qadd8(fb_b[strip_id][led], color.b);
        }
    }
}

static void noise_update(double time_ms, PixelFunc pixel,
                         const Palette16 palette) {
    double mod1 = noise_mod1(time_ms);
    double mod2 = noise_mod2(time_ms);

    // Map [-1,1] oscillators to speed/scale ranges
    int minSpeed = 5, maxSpeed = 20;
    int minScale = 1, maxScale = 10;
    int speed = (int)(minSpeed + mod2 * (maxSpeed - minSpeed));
    int nscale = (int)(minScale + mod1 * (maxScale - minScale));

    fb_fade_all();
    fill_noise(speed, nscale);

    // ihue rolls once per call across all strips (mirrors the static in the
    // original map_noise_to_leds_with_palette)
    static uint8_t ihue = 0;

    // Matrix: 8 rows × 32 columns
    map_noise_to_strip(4, 8, 32, 0,   ihue, palette); ihue++;
    // Bars: 1 row × 142 columns, each with a different noise offset
    map_noise_to_strip(0, 1, get_strip_num_leds(0),  50,  ihue, palette); ihue++;
    map_noise_to_strip(1, 1, get_strip_num_leds(1), 100,  ihue, palette); ihue++;
    map_noise_to_strip(2, 1, get_strip_num_leds(2), 150,  ihue, palette); ihue++;
    map_noise_to_strip(3, 1, get_strip_num_leds(3),  20,  ihue, palette); ihue++;
    // WS2801 strips
    map_noise_to_strip(5, 1, get_strip_num_leds(5),  50,  ihue, palette); ihue++;
    map_noise_to_strip(6, 1, get_strip_num_leds(6), 100,  ihue, palette); ihue++;

    // Flush frame buffer to the visualizer
    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            pixel(s, i, &fb_r[s][i], &fb_g[s][i], &fb_b[s][i]);
        }
    }
}
