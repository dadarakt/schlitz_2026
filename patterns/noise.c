// Noise pattern -- ported from schlitzerei/src/patterns/NoisePatterns.cpp
//
// Diverges from the original in two ways, both intentional (see the comments
// at map_noise_to_bar and advance_noise_walk below):
//   - 1D strips sample inoise8() directly per LED instead of going through
//     the 32-wide matrix noise table, so they don't repeat every 32 LEDs.
//   - the noise-walk speed is driven by elapsed real time instead of calls
//     per frame, so it doesn't depend on the achieved frame rate.

#include "shared.h"
#include <led_viz.h>

#define NOISE_DIM 32 // MATRIX_MAX_DIMENSION = max(32, 8) -- matrix only

// Per-frame fade amount for the Noise pattern specifically -- Patterns.cpp
// sets decay_rate=50 while Noise is active (not the global default of 100).
// FastLED's fadeToBlackBy(50) scales each channel via nscale8(255-50)=205.
#define FADE_SCALE 205

// Unlike the segment/gap-based patterns (Count Up, Wave, ...), every LED is
// lit at once here, so the installation reads noticeably brighter overall
// even though individual pixel brightness looks similar. Scale the final
// per-pixel brightness down uniformly to compensate -- keeps the noise's own
// bright/dark contrast intact, just lowers the overall level.
#define NOISE_BRIGHTNESS_SCALE 150 // out of 255

// Frame buffers – persist across frames to implement fade + additive blending
#define FB_MAX_LEDS 256
static uint8_t fb_r[7][FB_MAX_LEDS];
static uint8_t fb_g[7][FB_MAX_LEDS];
static uint8_t fb_b[7][FB_MAX_LEDS];

// 2D noise field -- only used for the matrix (exactly NOISE_DIM wide, so no
// wraparound repeat). Kept as doubles so many small per-frame advances don't
// lose precision the way accumulating into an int would.
static uint8_t noise_field[NOISE_DIM][NOISE_DIM];

// Noise walk coordinates
static double noise_x = 12345.0;
static double noise_y = 54321.0;
static double noise_z = 11111.0;
static double last_time_ms = -1.0;

// Expand inoise8's ~16-238 range toward 0-255.
static uint8_t expand_noise(uint8_t data) {
  data = qsub8(data, 16);
  return qadd8(data, scale8(data, 39));
}

// get_strip_position() is a normalized -1..1 location along one shared 1D
// spatial axis for the whole installation; get_strip_length_cm() is each
// strip's own physical extent along that axis. There's no API for the
// installation's real total width, so this sets how many cm the full -1..1
// position range spans -- tune to match your actual hardware layout.
#define INSTALLATION_HALF_WIDTH_CM 200.0

// Absolute cm coordinate of LED i (of n) on strip_id, shared across the
// whole installation, so adjacent strips sample continuous noise instead of
// each restarting from its own local zero.
static double led_position_cm(int strip_id, int i, int n) {
  double frac = (n > 1) ? (double)i / (double)(n - 1) : 0.0;
  return get_strip_position(strip_id) * INSTALLATION_HALF_WIDTH_CM +
         frac * get_strip_length_cm(strip_id);
}

// Advance the shared noise-walk coordinates by *elapsed real time* rather
// than a fixed amount per call. The original targets fps=120 (Globals.h),
// but realistically never hit that driving 976 LEDs of bit-banged WS2812 --
// more like 30-40fps. Our ESP32 build can comfortably hit close to its
// target_fps with far fewer strips active, so a fixed per-call step made the
// walk (and thus the whole animation) run too fast relative to elapsed time.
// SPEED_SCALE is an extra overall multiplier on top of the elapsed-time
// correction, tuned by eye against real hardware -- adjust freely.
#define REFERENCE_FRAME_MS (1000.0 / 120.0)
#define SPEED_SCALE 0.5
static void advance_noise_walk(int speed, double time_ms) {
  double frames = (last_time_ms < 0.0) ? 1.0
                                       : (time_ms - last_time_ms) /
                                             REFERENCE_FRAME_MS * SPEED_SCALE;
  last_time_ms = time_ms;

  noise_z += speed / 2.0 * frames;
  noise_x += speed / 4.0 * frames;
  noise_y -= speed / 8.0 * frames;
}

// Recompute the matrix's 32x32 noise field from the current walk position.
static void fill_noise_matrix(int strip_id, int nscale) {
  // Blend each new sample with the previous value so the field evolves
  // smoothly frame-to-frame instead of jumping every frame (mirrors
  // NoisePatterns.cpp's fill_noise_8). Note: in the original, data_smoothing
  // is a global initialized once from the *startup* speed value (5), not
  // recomputed per frame despite speed changing -- replicated exactly.
  static const uint8_t data_smoothing = 180; // 200 - (5 * 4)

  // Horizontal axis uses the matrix's own physical position/width so it
  // lines up with the bars on either side of it; the vertical axis has no
  // cross-strip adjacency to worry about, so it stays purely local.
  double base_cm = get_strip_position(strip_id) * INSTALLATION_HALF_WIDTH_CM;
  double width_cm = get_strip_length_cm(strip_id);
  double cm_per_col = (NOISE_DIM > 1) ? width_cm / (NOISE_DIM - 1) : 0.0;

  for (int i = 0; i < NOISE_DIM; i++) {
    double i_off = nscale * (base_cm + i * cm_per_col);
    for (int j = 0; j < NOISE_DIM; j++) {
      int j_off = nscale * j;
      uint8_t data =
          expand_noise(inoise8((uint16_t)(noise_x + i_off),
                               (uint16_t)(noise_y + j_off), (uint16_t)noise_z));

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

// Each frame adds a freshly-sampled color on top of the still-fading
// leftover from previous frames (see fb_fade_all/qadd8 below) -- since the
// noise walk rotates the hue over time, a pixel can end up with several
// different hues' contributions summed together before they fully decay,
// pushing all three channels up at once and reading as a wash of white
// rather than a blend of colors. Subtracting the shared (achromatic)
// component back out restores saturation without touching the relative
// hue difference between channels.
static void fb_desaturate(uint8_t *r, uint8_t *g, uint8_t *b) {
  uint8_t m = *r;
  if (*g < m) m = *g;
  if (*b < m) m = *b;
  *r = (uint8_t)(*r - m);
  *g = (uint8_t)(*g - m);
  *b = (uint8_t)(*b - m);
}

// Matrix: sample from the precomputed 32-wide noise field -- fits exactly,
// so no wraparound/repeat.
static void map_noise_to_matrix(int strip_id, int width, int height,
                                uint8_t ihue, const Palette16 palette) {
  for (int i = 0; i < width; i++) {
    for (int j = 0; j < height; j++) {
      uint8_t index = noise_field[j % NOISE_DIM][i % NOISE_DIM];
      uint8_t bri = noise_field[i % NOISE_DIM][j % NOISE_DIM];

      // Plain wrapping add (not qadd8): index is a rotating hue offset into
      // a cyclic 0-255 palette space, so it must wrap like an odometer, not
      // saturate at 255. Saturating here clamped every pixel to the same
      // top-of-range palette entry right before each wrap, then released
      // back to real values right after -- a visible hard cut every rotation.
      index = (uint8_t)(index + ihue);
      bri = (bri > 127) ? 255 : dim8_raw((uint8_t)(bri * 2));
      bri = scale8(bri, NOISE_BRIGHTNESS_SCALE);

      RGB color = palette_sample(palette, index, bri, true);
      int led = get_matrix_index(strip_id, i, j);

      fb_r[strip_id][led] = qadd8(fb_r[strip_id][led], color.r);
      fb_g[strip_id][led] = qadd8(fb_g[strip_id][led], color.g);
      fb_b[strip_id][led] = qadd8(fb_b[strip_id][led], color.b);
      fb_desaturate(&fb_r[strip_id][led], &fb_g[strip_id][led], &fb_b[strip_id][led]);
    }
  }
}

// 1D strips (bars, WS2801 strips): sample inoise8() directly per LED across
// the strip's full length, instead of the 32-wide matrix table -- the table
// is far shorter than a 142-LED bar, so indexing it with (led % 32) just
// repeats the same 32 values ~4.4 times along the strip. index and bri are
// sampled along different axes of the walk (mirrors the original's use of
// transposed lookups into the same field) so they're decorrelated. Position
// comes from led_position_cm(), so adjacent strips see continuous noise
// across the physical gap between them instead of each restarting locally.
static void map_noise_to_bar(int strip_id, int nscale, uint8_t ihue,
                             const Palette16 palette) {
  int n = get_strip_num_leds(strip_id);
  for (int i = 0; i < n; i++) {
    double pos = (double)nscale * led_position_cm(strip_id, i, n);

    uint8_t index = expand_noise(inoise8((uint16_t)(noise_x + pos),
                                         (uint16_t)noise_y, (uint16_t)noise_z));
    uint8_t bri = expand_noise(inoise8(
        (uint16_t)noise_x, (uint16_t)(noise_y + pos), (uint16_t)noise_z));

    // Plain wrapping add (not qadd8) -- see map_noise_to_matrix for why.
    index = (uint8_t)(index + ihue);
    bri = (bri > 127) ? 255 : dim8_raw((uint8_t)(bri * 2));
    bri = scale8(bri, NOISE_BRIGHTNESS_SCALE);

    RGB color = palette_sample(palette, index, bri, true);

    fb_r[strip_id][i] = qadd8(fb_r[strip_id][i], color.r);
    fb_g[strip_id][i] = qadd8(fb_g[strip_id][i], color.g);
    fb_b[strip_id][i] = qadd8(fb_b[strip_id][i], color.b);
    fb_desaturate(&fb_r[strip_id][i], &fb_g[strip_id][i], &fb_b[strip_id][i]);
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
  advance_noise_walk(speed, time_ms);
  fill_noise_matrix(4, nscale);

  // Rolling hue offset for palette cycling, one full 256-step rotation every
  // HUE_CYCLE_MS. Derived from elapsed time rather than a per-call counter:
  // a per-call increment (the original's static ihue, +1 per strip) ties
  // the rotation rate to whatever frame rate happens to be achieved -- at
  // 7 strips/frame and ~60fps that wrapped a uint8_t roughly every 0.6s,
  // fast enough that sweeping through a palette's bright/dark regions read
  // as a fixed-frequency brightness pulse. HUE_CYCLE_MS is freely tunable.
#define HUE_CYCLE_MS 8000.0
  uint8_t base_hue =
      (uint8_t)(fmod(time_ms, HUE_CYCLE_MS) / HUE_CYCLE_MS * 256.0);

  // Matrix: 8 rows × 32 columns
  map_noise_to_matrix(4, 32, 8, base_hue, palette);
  // Bars + WS2801 strips: position-derived, small per-strip hue offset for
  // variety (matches the original's per-strip ihue spread within one frame)
  map_noise_to_bar(0, nscale, (uint8_t)(base_hue + 1), palette);
  map_noise_to_bar(1, nscale, (uint8_t)(base_hue + 2), palette);
  map_noise_to_bar(2, nscale, (uint8_t)(base_hue + 3), palette);
  map_noise_to_bar(3, nscale, (uint8_t)(base_hue + 4), palette);
  map_noise_to_bar(5, nscale, (uint8_t)(base_hue + 5), palette);
  map_noise_to_bar(6, nscale, (uint8_t)(base_hue + 6), palette);

  // Flush frame buffer to the visualizer
  for (int s = 0; s < 7; s++) {
    int n = get_strip_num_leds(s);
    for (int i = 0; i < n; i++) {
      pixel(s, i, &fb_r[s][i], &fb_g[s][i], &fb_b[s][i]);
    }
  }
}
