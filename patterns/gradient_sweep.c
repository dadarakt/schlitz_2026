// Gradient Sweep pattern -- new pattern. Bars + matrix share one two-color
// gradient (sampled from the current palette) and light up in a horizontal
// chase across the installation's physical left-to-right layout: bar1,
// bar2, matrix, bar3, bar4. Direction (left-to-right or right-to-left) is
// re-rolled each pass. Rather than a full reveal-then-reset, a sliding
// window keeps only 2-3 stops lit at once -- as the chase advances, the
// newest stop fades in while the oldest one in the window fades out, so at
// most a handful are ever on simultaneously. Each stop's brightness eases
// smoothly toward on/off (not an instant flip) for a more fluid look. Once
// a full pass finishes, there's a brief all-off gap before a fresh pass
// starts with new colors, a new random direction, and a new window size.
// Step speed is modulated by mod1/mod2 plus randomness.
//
// Bars show the gradient bottom to top (vertical); the matrix shows it
// left to right across its columns (horizontal), uniform down each column
// -- both assuming index/column 0 is the physically "low"/"left" end.
// WS2801 strips aren't part of this pattern, and are cleared to black each
// frame so nothing stale lingers from whichever pattern ran before this.
//
// Unlike Count Up, every stop here shares one single chase state (colors,
// direction, window size, step timing) -- there's no per-stop independent
// variety to begin with, so *all* of its randomness is derived via
// deterministic_rand (see shared.h), seeded from schedule timestamps
// rather than "now". That keeps a multi-board mesh-sync setup's two
// halves of the chase showing the same colors and moving in the same
// direction at the same time, with zero extra network messages.

#include "shared.h"
#include <led_viz.h>

// Physical left-to-right order: bar1, bar2, matrix (id 4), bar3, bar4.
#define GS_NUM_STOPS 5
static const int gs_stop_ids[GS_NUM_STOPS] = {0, 1, 4, 2, 3};

#define GS_MIN_STEP_MS 180.0
#define GS_MAX_STEP_MS 480.0
#define GS_GAP_MS 300.0 // brief all-off pause between passes

#define GS_MIN_WINDOW 2
#define GS_MAX_WINDOW 3

#define GS_FADE_MS 160.0 // time to ease a stop fully on or fully off

// Quantization for the very first cycle's seed (see shared.h's
// quantize_sync_ms) -- generous relative to typical ESP-NOW propagation
// delay + per-board frame polling jitter.
#define GS_SYNC_QUANTIZE_MS 250.0

static RGB gs_color_a = {0, 0, 0};
static RGB gs_color_b = {0, 0, 0};
static int gs_direction = 1;   // +1 = left to right, -1 = right to left
static int gs_window_size = GS_MIN_WINDOW;
static int gs_level = 0;       // sweep position, 0..(GS_NUM_STOPS+window)
static double gs_next_step_ms = 0.0;
static double gs_last_frame_ms = -1.0;
static float gs_brightness[GS_NUM_STOPS] = {0}; // per-stop 0..255, eased
static bool gs_started = false;

// seed_ms is always an already-*agreed* schedule timestamp (never "now")
// -- mod1/mod2 are recomputed from it here rather than passed in from the
// caller's live time_ms, so the result is bit-identical across boards with
// no dependency on per-board frame-timing jitter at all.
static double gs_step_ms(double seed_ms) {
    double mod1 = noise_mod1(seed_ms);
    double mod2 = noise_mod2(seed_ms);
    double norm = ((mod1 + mod2) * 0.5 + 1.0) * 0.5; // combine both, 0..1
    double step = GS_MIN_STEP_MS + norm * (GS_MAX_STEP_MS - GS_MIN_STEP_MS);
    uint32_t r = deterministic_rand(seed_ms, 5);
    step += (double)(r % 161) - 80.0; // +-80ms randomness
    if (step < 80.0) step = 80.0;
    return step;
}

static void gs_new_cycle(double seed_ms, const Palette16 palette) {
    uint8_t hue_a = (uint8_t)(deterministic_rand(seed_ms, 1) & 0xFF);
    // Offset by roughly a third to two-thirds of the wheel for a clearly
    // contrasting second color, with some variety in exactly how far.
    uint8_t hue_b = (uint8_t)(hue_a + 96 + (deterministic_rand(seed_ms, 2) % 64));
    gs_color_a = palette_sample(palette, hue_a, 255, true);
    gs_color_b = palette_sample(palette, hue_b, 255, true);
    gs_direction = (deterministic_rand(seed_ms, 3) & 1) ? 1 : -1;
    gs_window_size = GS_MIN_WINDOW + (deterministic_rand(seed_ms, 4) %
                                      (GS_MAX_WINDOW - GS_MIN_WINDOW + 1));
    gs_level = 0;
    gs_next_step_ms = seed_ms + gs_step_ms(seed_ms);
    for (int i = 0; i < GS_NUM_STOPS; i++) gs_brightness[i] = 0.0f;
}

static RGB gs_lerp(RGB a, RGB b, float t) {
    RGB c;
    c.r = (uint8_t)(a.r + (b.r - a.r) * t);
    c.g = (uint8_t)(a.g + (b.g - a.g) * t);
    c.b = (uint8_t)(a.b + (b.b - a.b) * t);
    return c;
}

static RGB gs_scale_color(RGB c, uint8_t level) {
    RGB out;
    out.r = scale8(c.r, level);
    out.g = scale8(c.g, level);
    out.b = scale8(c.b, level);
    return out;
}

static void gs_render_bar(int strip_id, uint8_t level, PixelFunc pixel) {
    int n = get_strip_num_leds(strip_id);
    for (int i = 0; i < n; i++) {
        float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        RGB c = gs_scale_color(gs_lerp(gs_color_a, gs_color_b, t), level);
        pixel(strip_id, i, &c.r, &c.g, &c.b);
    }
}

static void gs_render_matrix(int strip_id, uint8_t level, PixelFunc pixel) {
    int width = get_matrix_width(strip_id);
    int height = get_matrix_height(strip_id);
    if (width <= 0 || height <= 0) return;

    for (int x = 0; x < width; x++) {
        float t = (width > 1) ? (float)x / (float)(width - 1) : 0.0f;
        RGB c = gs_scale_color(gs_lerp(gs_color_a, gs_color_b, t), level);
        for (int y = 0; y < height; y++) {
            int led = get_matrix_index(strip_id, x, y);
            pixel(strip_id, led, &c.r, &c.g, &c.b);
        }
    }
}

static void gs_clear(int strip_id, PixelFunc pixel) {
    int n = get_strip_num_leds(strip_id);
    RGB black = {0, 0, 0};
    for (int i = 0; i < n; i++) {
        pixel(strip_id, i, &black.r, &black.g, &black.b);
    }
}

static void gs_init(void) {
    gs_color_a = (RGB){0, 0, 0};
    gs_color_b = (RGB){0, 0, 0};
    gs_direction = 1;
    gs_window_size = GS_MIN_WINDOW;
    gs_level = 0;
    gs_next_step_ms = 0.0;
    gs_last_frame_ms = -1.0;
    for (int i = 0; i < GS_NUM_STOPS; i++) gs_brightness[i] = 0.0f;
    gs_started = false;
}

static void gradient_sweep_update(double time_ms, PixelFunc pixel,
                                  const Palette16 palette) {
    // The very first cycle seeds from *quantized* real time (see shared.h)
    // since this is the one place "now" legitimately has to enter the
    // chain -- every decision after that seeds from the schedule itself,
    // never from "now" again.
    if (!gs_started) {
        gs_new_cycle(quantize_sync_ms(time_ms, GS_SYNC_QUANTIZE_MS), palette);
        gs_started = true;
    }

    int max_level = GS_NUM_STOPS + gs_window_size;

    if (time_ms >= gs_next_step_ms) {
        // The schedule timestamp just reached, agreed on last cycle --
        // seed from this, not the current frame's time_ms.
        double event_time = gs_next_step_ms;
        if (gs_level >= max_level) {
            // Pass finished -- gap, then a fresh pass with new colors,
            // direction, and window size.
            gs_new_cycle(event_time + GS_GAP_MS, palette);
        } else {
            gs_level++;
            gs_next_step_ms = event_time + gs_step_ms(event_time);
        }
    }

    double dt_ms = (gs_last_frame_ms < 0.0) ? 16.0 : (time_ms - gs_last_frame_ms);
    gs_last_frame_ms = time_ms;
    float fade_amount = (float)(255.0 * dt_ms / GS_FADE_MS);

    for (int k = 0; k < GS_NUM_STOPS; k++) {
        // How far along the *current sweep direction* stop k is, so
        // direction can flip cheaply without reordering gs_stop_ids.
        int order = (gs_direction > 0) ? k : (GS_NUM_STOPS - 1 - k);
        bool target_on = (order < gs_level) && (order >= gs_level - gs_window_size);
        float target = target_on ? 255.0f : 0.0f;

        if (gs_brightness[k] < target) {
            gs_brightness[k] += fade_amount;
            if (gs_brightness[k] > target) gs_brightness[k] = target;
        } else if (gs_brightness[k] > target) {
            gs_brightness[k] -= fade_amount;
            if (gs_brightness[k] < target) gs_brightness[k] = target;
        }

        int strip_id = gs_stop_ids[k];
        uint8_t level = (uint8_t)(gs_brightness[k] + 0.5f);
        if (strip_id == 4) {
            gs_render_matrix(strip_id, level, pixel);
        } else {
            gs_render_bar(strip_id, level, pixel);
        }
    }

    // Not part of this pattern -- clear so nothing stale lingers.
    gs_clear(5, pixel);
    gs_clear(6, pixel);
}
