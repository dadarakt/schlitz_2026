// Count Up pattern -- new pattern (not ported from the original schlitzerei
// codebase). Every strip -- bars, WS2801 strips, and the matrix -- is
// divided into 8 "segments" and all of them run the SAME mode at once,
// switching together over time:
//   - CU_MODE_UP:     segments fill in sequence from the bottom, hold
//                      briefly once full, then reset to empty and repeat.
//   - CU_MODE_DOWN:   the mirror image -- fills from the top down instead.
//   - CU_MODE_RANDOM: segments are randomly turned on/off, refreshed
//                      periodically, instead of filling in sequence.
// The whole installation picks a random duration (a handful of seconds) to
// stay in whatever mode it's in, then all panels switch to a new (usually
// different) mode together and each picks a fresh palette color -- so the
// installation drifts through different textures over time as one, while
// individual strips still vary in their own step timing/color so it doesn't
// look perfectly synchronized.
//
// On 1D strips (bars, WS2801 strips), a segment is a run of LEDs separated
// by a few pixels of black, computed from the strip's actual LED count.
// On the matrix, the 8 segments are laid out as a 2-row x 4-column grid of
// rectangular patches, also separated by a couple of black pixels.
//
// "Bottom" is assumed to be LED index 0 on 1D strips, and low Y on the
// matrix; if a strip's physical wiring runs the other way, that's the one
// assumption to flip for it specifically.

#include "shared.h"
#include <led_viz.h>
#include <stdlib.h>

#define CU_NUM_SEGMENTS 8
#define CU_GAP_PIXELS 3

#define CU_MIN_STEP_MS 90.0
#define CU_MAX_STEP_MS 260.0
#define CU_HOLD_FULL_MS 300.0 // brief pause once fully lit, before reset

// How long the whole installation stays in one mode before switching.
#define CU_MODE_DURATION_MIN_MS 6000.0
#define CU_MODE_DURATION_MAX_MS 15000.0

// Quantization for the *first* mode decision's seed (see shared.h's
// quantize_sync_ms) -- generous relative to typical ESP-NOW propagation
// delay + per-board frame polling jitter, so two mesh-synced boards
// switching into this pattern a few ms apart still agree on the same
// starting mode/duration.
#define CU_SYNC_QUANTIZE_MS 250.0

// Matrix patch grid -- must multiply out to CU_NUM_SEGMENTS.
#define CU_MATRIX_COLS 4
#define CU_MATRIX_ROWS 2
#define CU_MATRIX_GAP_PIXELS 2

// Indexed directly by strip id (0-3 bars, 4 matrix, 5-6 WS2801 strips)
#define CU_NUM_STRIP_SLOTS 7

typedef enum { CU_MODE_UP, CU_MODE_DOWN, CU_MODE_RANDOM, CU_MODE_COUNT } CUMode;

typedef struct {
    int level;             // 0..CU_NUM_SEGMENTS currently lit, for UP/DOWN
    uint8_t random_mask;    // bit i = segment i lit, for RANDOM
    double next_step_ms;    // next count-step / random-refresh time
    RGB color;
} CUStrip;

static CUStrip cu_state[CU_NUM_STRIP_SLOTS];
static CUMode cu_global_mode = CU_MODE_UP;
static double cu_next_mode_change_ms = 0.0;
static bool cu_times_staggered = false;

// Compute all 8 segments' (start, length) for a 1D strip of n LEDs, with
// CU_GAP_PIXELS of black between each and any remainder pixels spread
// across segments so the strip is used edge to edge. Falls back to no gaps
// at all if the strip is too short to fit full gaps.
static void cu_compute_segments(int n, int starts[CU_NUM_SEGMENTS],
                                int lens[CU_NUM_SEGMENTS]) {
    int gap = CU_GAP_PIXELS;
    int available = n - gap * (CU_NUM_SEGMENTS - 1);
    if (available < CU_NUM_SEGMENTS) {
        available = n;
        gap = 0;
    }
    int base = available / CU_NUM_SEGMENTS;
    int extra = available % CU_NUM_SEGMENTS;
    int pos = 0;
    for (int i = 0; i < CU_NUM_SEGMENTS; i++) {
        int len = base + (i < extra ? 1 : 0);
        starts[i] = pos;
        lens[i] = len;
        pos += len + gap;
    }
}

// Splits `total` into `count` spans of near-equal length, separated by
// `gap` pixels, falling back to no gap if too short to fit. Used for both
// the matrix's columns and its rows.
static void cu_split_with_gap(int total, int count, int gap, int *starts,
                              int *lens) {
    int available = total - gap * (count - 1);
    if (available < count) {
        available = total;
        gap = 0;
    }
    int base = available / count;
    int extra = available % count;
    int pos = 0;
    for (int i = 0; i < count; i++) {
        int len = base + (i < extra ? 1 : 0);
        starts[i] = pos;
        lens[i] = len;
        pos += len + gap;
    }
}

// Compute the matrix's 8 patches as a CU_MATRIX_ROWS x CU_MATRIX_COLS grid,
// patch index = row * CU_MATRIX_COLS + col. Row 0 is low Y (bottom), so
// patches 0..COLS-1 are the bottom row -- "count up" fills bottom row first.
static void cu_compute_matrix_patches(int width, int height,
                                      int x0[CU_NUM_SEGMENTS], int x1[CU_NUM_SEGMENTS],
                                      int y0[CU_NUM_SEGMENTS], int y1[CU_NUM_SEGMENTS]) {
    int col_start[CU_MATRIX_COLS], col_len[CU_MATRIX_COLS];
    cu_split_with_gap(width, CU_MATRIX_COLS, CU_MATRIX_GAP_PIXELS, col_start, col_len);

    int row_start[CU_MATRIX_ROWS], row_len[CU_MATRIX_ROWS];
    cu_split_with_gap(height, CU_MATRIX_ROWS, CU_MATRIX_GAP_PIXELS, row_start, row_len);

    for (int r = 0; r < CU_MATRIX_ROWS; r++) {
        for (int c = 0; c < CU_MATRIX_COLS; c++) {
            int p = r * CU_MATRIX_COLS + c;
            x0[p] = col_start[c];
            x1[p] = col_start[c] + col_len[c];
            y0[p] = row_start[r];
            y1[p] = row_start[r] + row_len[r];
        }
    }
}

static double cu_step_ms(int strip_id, double mod1, double mod2) {
    double m = (strip_id % 2 == 0) ? mod1 : mod2; // alternate per strip for variety
    double norm = (m + 1.0) * 0.5;                 // 0..1
    double step = CU_MIN_STEP_MS + norm * (CU_MAX_STEP_MS - CU_MIN_STEP_MS);
    step += (double)(rand() % 61) - 30.0; // +-30ms randomness
    if (step < 30.0) step = 30.0;
    return step;
}

// The global mode and its switch timing are the one piece of state every
// panel -- and, in a multi-board mesh-sync setup, every board -- shares,
// so unlike each strip's own step timing/color (still plain rand(), still
// meant to vary independently), these two derive from deterministic_rand
// seeded by an already-*agreed* schedule timestamp rather than "now". See
// shared.h's deterministic_rand for why that distinction matters.
static double cu_mode_duration(double seed_ms) {
    double span = CU_MODE_DURATION_MAX_MS - CU_MODE_DURATION_MIN_MS;
    uint32_t r = deterministic_rand(seed_ms, 1);
    return CU_MODE_DURATION_MIN_MS + (double)(r % (uint32_t)span);
}

// Picks a new mode, avoiding an immediate repeat of `avoid` for more
// noticeable variety when the installation switches.
static CUMode cu_pick_mode(CUMode avoid, double seed_ms) {
    uint32_t r = deterministic_rand(seed_ms, 2);
    CUMode next = (CUMode)(r % CU_MODE_COUNT);
    if (next == avoid) {
        next = (CUMode)((next + 1) % CU_MODE_COUNT);
    }
    return next;
}

static void cu_reset_strip(CUStrip *s, double time_ms, const Palette16 palette) {
    s->level = 0;
    s->random_mask = 0;
    s->color = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
    s->next_step_ms = time_ms;
}

static void cu_advance(CUStrip *s, int strip_id, double time_ms, double mod1,
                       double mod2, const Palette16 palette) {
    if (time_ms < s->next_step_ms) return;

    double step = cu_step_ms(strip_id, mod1, mod2);

    if (cu_global_mode == CU_MODE_RANDOM) {
        s->random_mask = (uint8_t)(rand() & 0xFF);
        s->next_step_ms = time_ms + step;
        return;
    }

    // UP / DOWN share this same counting state machine; only rendering
    // interprets `level` differently depending on direction.
    if (s->level >= CU_NUM_SEGMENTS) {
        s->level = 0;
        s->color = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
        s->next_step_ms = time_ms + step;
    } else {
        s->level++;
        s->next_step_ms =
            time_ms + (s->level >= CU_NUM_SEGMENTS ? CU_HOLD_FULL_MS : step);
    }
}

static bool cu_segment_lit(const CUStrip *s, int seg) {
    switch (cu_global_mode) {
        case CU_MODE_UP:     return seg < s->level;
        case CU_MODE_DOWN:   return seg >= (CU_NUM_SEGMENTS - s->level);
        case CU_MODE_RANDOM: return (s->random_mask >> seg) & 1;
        default:              return false;
    }
}

static void cu_render_strip(int strip_id, const CUStrip *s, PixelFunc pixel) {
    int n = get_strip_num_leds(strip_id);
    if (n <= 0) return;

    int starts[CU_NUM_SEGMENTS], lens[CU_NUM_SEGMENTS];
    cu_compute_segments(n, starts, lens);

    for (int i = 0; i < n; i++) {
        RGB c = {0, 0, 0};
        for (int seg = 0; seg < CU_NUM_SEGMENTS; seg++) {
            if (i >= starts[seg] && i < starts[seg] + lens[seg]) {
                if (cu_segment_lit(s, seg)) c = s->color;
                break;
            }
        }
        pixel(strip_id, i, &c.r, &c.g, &c.b);
    }
}

static void cu_render_matrix(int strip_id, const CUStrip *s, PixelFunc pixel) {
    int width = get_matrix_width(strip_id);
    int height = get_matrix_height(strip_id);
    if (width <= 0 || height <= 0) return;

    int x0[CU_NUM_SEGMENTS], x1[CU_NUM_SEGMENTS];
    int y0[CU_NUM_SEGMENTS], y1[CU_NUM_SEGMENTS];
    cu_compute_matrix_patches(width, height, x0, x1, y0, y1);

    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            RGB c = {0, 0, 0};
            for (int p = 0; p < CU_NUM_SEGMENTS; p++) {
                if (x >= x0[p] && x < x1[p] && y >= y0[p] && y < y1[p]) {
                    if (cu_segment_lit(s, p)) c = s->color;
                    break;
                }
            }
            int led = get_matrix_index(strip_id, x, y);
            pixel(strip_id, led, &c.r, &c.g, &c.b);
        }
    }
}

static void cu_init(void) {
    for (int i = 0; i < CU_NUM_STRIP_SLOTS; i++) {
        cu_state[i].level = 0;
        cu_state[i].random_mask = 0;
        cu_state[i].next_step_ms = 0.0;
        cu_state[i].color = (RGB){0, 0, 0};
    }
    cu_global_mode = CU_MODE_UP;
    cu_next_mode_change_ms = 0.0;
    cu_times_staggered = false;
}

static void count_up_update(double time_ms, PixelFunc pixel,
                            const Palette16 palette) {
    double mod1 = noise_mod1(time_ms);
    double mod2 = noise_mod2(time_ms);

    static const int strip_ids[] = {0, 1, 2, 3, 4, 5, 6};
    const int active_count = (int)(sizeof(strip_ids) / sizeof(strip_ids[0]));

    // Stagger each strip's first step relative to actual entry time, rather
    // than all of them firing together on the first frame this pattern runs.
    //
    // The very first mode decision seeds from *quantized* real time (see
    // shared.h) since this is the one place "now" legitimately has to enter
    // the chain -- every subsequent decision below seeds from the schedule
    // itself, never from "now" again, so a multi-board setup that agrees
    // here stays in lockstep indefinitely without needing to re-agree.
    if (!cu_times_staggered) {
        double seed = quantize_sync_ms(time_ms, CU_SYNC_QUANTIZE_MS);
        cu_global_mode = cu_pick_mode((CUMode)-1, seed);
        cu_next_mode_change_ms = seed + cu_mode_duration(seed);
        for (int k = 0; k < active_count; k++) {
            cu_reset_strip(&cu_state[strip_ids[k]], time_ms + k * 120.0, palette);
        }
        cu_times_staggered = true;
    }

    // All panels switch mode together. Seed from the schedule timestamp
    // just reached (agreed on last cycle), not the current frame's
    // time_ms -- see shared.h's deterministic_rand.
    if (time_ms >= cu_next_mode_change_ms) {
        double seed = cu_next_mode_change_ms;
        cu_global_mode = cu_pick_mode(cu_global_mode, seed);
        cu_next_mode_change_ms = seed + cu_mode_duration(seed);
        for (int k = 0; k < active_count; k++) {
            cu_reset_strip(&cu_state[strip_ids[k]], time_ms, palette);
        }
    }

    for (int k = 0; k < active_count; k++) {
        int id = strip_ids[k];
        cu_advance(&cu_state[id], id, time_ms, mod1, mod2, palette);
        if (id == 4) {
            cu_render_matrix(id, &cu_state[id], pixel);
        } else {
            cu_render_strip(id, &cu_state[id], pixel);
        }
    }
}
