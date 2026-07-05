// Count Up pattern -- new pattern (not ported from the original schlitzerei
// codebase). Every strip -- bars, WS2801 strips, and the matrix -- is
// divided into a number of "segments" and all of them run the SAME mode at
// once, switching together over time:
//   - CU_MODE_UP:     segments fill in sequence from the bottom, hold
//                      briefly once full, then reset to empty and repeat.
//   - CU_MODE_DOWN:   the mirror image -- fills from the top down instead.
//   - CU_MODE_RANDOM: segments are randomly turned on/off, refreshed
//                      periodically, instead of filling in sequence.
//   - CU_MODE_SLIDE:  a short window of CU_SLIDE_WIDTH_MIN..MAX segments
//                      (never the whole strip) bounces back and forth
//                      between the two ends instead of filling/emptying.
// The whole installation picks a random, deliberately long duration to
// stay in whatever mode it's in (calm, unhurried counting rather than
// frequent mode changes), then all panels switch to a new (usually
// different) mode together and each picks a fresh palette color -- so the
// installation drifts through different textures over time as one, while
// individual strips still vary in their own step timing/color so it doesn't
// look perfectly synchronized -- *unless* lockstep gets rolled for the new
// cycle (see CU_LOCKSTEP_CHANCE_PERCENT), in which case every panel shares
// one single state instead: same color, same level, same phase, so the
// whole installation pulses as one for a while before the next mode change
// has a chance to revert to independent variety again.
//
// The segment count itself isn't fixed either -- it breathes slowly between
// CU_MIN_SEGMENTS and CU_MAX_SEGMENTS and back (see cu_segment_count()), a
// smooth function of time alone so it stays trivially in sync across
// multiple mesh-synced boards with no extra state or decisions needed, the
// same way noise_mod1/mod2 already do. Only ever takes power-of-two values
// within that range -- not every integer in between -- so a change always
// reads as segments cleanly merging together or splitting apart, never an
// uneven one-at-a-time resize.
//
// On 1D strips (bars, WS2801 strips), a segment is a run of LEDs separated
// by a few pixels of black, computed from the strip's actual LED count. On
// the matrix, segments are laid out as a grid of rectangular patches (1 row
// when there are few enough to fit as simple vertical stripes, 2 rows once
// there are more than fit that way), also separated by a couple of black
// pixels.
//
// "Bottom" is assumed to be LED index 0 on 1D strips, and low Y on the
// matrix; if a strip's physical wiring runs the other way, that's the one
// assumption to flip for it specifically.

#include "shared.h"
#include <led_viz.h>
#include <stdlib.h>

#define CU_MIN_SEGMENTS 8
#define CU_MAX_SEGMENTS 16
// Both above must be powers of two -- cu_segment_count() only ever steps
// between them a power of two at a time (8, 16), never any integer in
// between, so the levels below are just log2(MIN)..log2(MAX).
#define CU_MIN_LEVEL 3
#define CU_MAX_LEVEL 4
// One full MIN->MAX->MIN cycle -- deliberately slow ("breathing"), not
// something meant to be watched step by step.
#define CU_SEGMENT_BREATHE_PERIOD_MS 60000.0
#define CU_TWO_PI 6.28318530717958647692

#define CU_GAP_PIXELS 3

#define CU_MIN_STEP_MS 90.0
#define CU_MAX_STEP_MS 260.0
#define CU_HOLD_FULL_MS 300.0 // brief pause once fully lit, before reset

// How long the whole installation stays in one mode before switching --
// deliberately long/calm rather than quick to change.
#define CU_MODE_DURATION_MIN_MS 18000.0
#define CU_MODE_DURATION_MAX_MS 35000.0

// CU_MODE_SLIDE's window width, in segments -- always well short of the
// whole strip (CU_MIN_SEGMENTS is 8), so it always reads as a short band
// moving along, never a fill.
#define CU_SLIDE_WIDTH_MIN 2
#define CU_SLIDE_WIDTH_MAX 3

// Chance (out of 100) that a given mode cycle also puts every panel into
// lockstep -- one shared state (color/level/phase) instead of each strip
// running its own. Rolled fresh alongside every mode change.
#define CU_LOCKSTEP_CHANCE_PERCENT 35

// Quantization for the *first* mode decision's seed (see shared.h's
// quantize_sync_ms) -- generous relative to typical ESP-NOW propagation
// delay + per-board frame polling jitter, so two mesh-synced boards
// switching into this pattern a few ms apart still agree on the same
// starting mode/duration.
#define CU_SYNC_QUANTIZE_MS 250.0

// Matrix patch grid: 1 row (simple vertical stripes) while everything fits
// that way, 2 rows once there are more segments than that.
#define CU_MATRIX_MAX_COLS_PER_ROW 8
#define CU_MATRIX_GAP_PIXELS 2

// Indexed directly by strip id (0-3 bars, 4 matrix, 5-6 WS2801 strips)
#define CU_NUM_STRIP_SLOTS 7

typedef enum {
    CU_MODE_UP,
    CU_MODE_DOWN,
    CU_MODE_RANDOM,
    CU_MODE_SLIDE,
    CU_MODE_COUNT
} CUMode;

typedef struct {
    int level;             // 0..num_segments currently lit (UP/DOWN), or the
                            // window's start position (SLIDE)
    int direction;          // +1/-1, which way the SLIDE window is currently
                            // moving (unused by other modes)
    uint16_t random_mask;   // bit i = segment i lit, for RANDOM (up to 16)
    double next_step_ms;    // next count-step / random-refresh time
    RGB color;
} CUStrip;

static CUStrip cu_state[CU_NUM_STRIP_SLOTS]; // used unless cu_lockstep
static CUStrip cu_shared_state;              // used while cu_lockstep
static bool cu_lockstep = false;
static int cu_slide_width = CU_SLIDE_WIDTH_MIN;

static CUMode cu_global_mode = CU_MODE_UP;
static double cu_next_mode_change_ms = 0.0;
static bool cu_times_staggered = false;

// Smooth function of (synced) time alone -- no rand()/extra state, so it's
// inherently mesh-sync-safe the same way noise_mod1/mod2 are: any number
// of boards evaluating this from the same synced clock agree automatically,
// with no decision to agree on in the first place.
//
// The underlying oscillation is still continuous, but it's quantized in
// log2 space to just the 4 power-of-two levels (2/4/8/16) instead of
// returning every integer in between -- so segment count only ever moves
// by a clean merge or split, holding steady at each level in between.
static int cu_segment_count(double time_ms) {
    double phase = time_ms * (CU_TWO_PI / CU_SEGMENT_BREATHE_PERIOD_MS);
    double t = (sin(phase) + 1.0) * 0.5; // 0..1
    int level = CU_MIN_LEVEL + (int)(t * (CU_MAX_LEVEL - CU_MIN_LEVEL) + 0.5);
    if (level < CU_MIN_LEVEL) level = CU_MIN_LEVEL;
    if (level > CU_MAX_LEVEL) level = CU_MAX_LEVEL;
    return 1 << level;
}

// Compute num_segments segments' (start, length) for a 1D strip of n LEDs,
// with CU_GAP_PIXELS of black between each and any remainder pixels spread
// across segments so the strip is used edge to edge. Falls back to no gaps
// at all if the strip is too short to fit full gaps.
static void cu_compute_segments(int n, int num_segments,
                                int starts[CU_MAX_SEGMENTS],
                                int lens[CU_MAX_SEGMENTS]) {
    int gap = CU_GAP_PIXELS;
    int available = n - gap * (num_segments - 1);
    if (available < num_segments) {
        available = n;
        gap = 0;
    }
    int base = available / num_segments;
    int extra = available % num_segments;
    int pos = 0;
    for (int i = 0; i < num_segments; i++) {
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

// Compute the matrix's num_segments patches as a grid: 1 row of simple
// vertical stripes while num_segments fits within a single row, else 2
// rows. patch index = row * cols + col, row 0 is low Y (bottom), so patch
// 0 is bottom-left -- "count up" fills that first. If num_segments is odd
// with 2 rows, the grid has one more slot than segments; that leftover
// cell is simply never assigned a patch and stays black.
static void cu_compute_matrix_patches(int width, int height, int num_segments,
                                      int x0[CU_MAX_SEGMENTS], int x1[CU_MAX_SEGMENTS],
                                      int y0[CU_MAX_SEGMENTS], int y1[CU_MAX_SEGMENTS]) {
    int rows = (num_segments <= CU_MATRIX_MAX_COLS_PER_ROW) ? 1 : 2;
    int cols = (num_segments + rows - 1) / rows; // ceil

    int col_start[CU_MAX_SEGMENTS], col_len[CU_MAX_SEGMENTS];
    cu_split_with_gap(width, cols, CU_MATRIX_GAP_PIXELS, col_start, col_len);

    int row_start[2], row_len[2];
    cu_split_with_gap(height, rows, CU_MATRIX_GAP_PIXELS, row_start, row_len);

    for (int p = 0; p < num_segments; p++) {
        int r = p / cols;
        int c = p % cols;
        x0[p] = col_start[c];
        x1[p] = col_start[c] + col_len[c];
        y0[p] = row_start[r];
        y1[p] = row_start[r] + row_len[r];
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

// The global mode/lockstep and their switch timing are the one piece of
// state every panel -- and, in a multi-board mesh-sync setup, every board
// -- shares, so unlike each strip's own step timing/color (still plain
// rand(), still meant to vary independently), these derive from
// deterministic_rand seeded by an already-*agreed* schedule timestamp
// rather than "now". See shared.h's deterministic_rand for why that
// distinction matters.
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

static bool cu_pick_lockstep(double seed_ms) {
    return (deterministic_rand(seed_ms, 3) % 100) < CU_LOCKSTEP_CHANCE_PERCENT;
}

static int cu_pick_slide_width(double seed_ms) {
    uint32_t span = CU_SLIDE_WIDTH_MAX - CU_SLIDE_WIDTH_MIN + 1;
    return CU_SLIDE_WIDTH_MIN + (int)(deterministic_rand(seed_ms, 4) % span);
}

static void cu_reset_strip(CUStrip *s, double time_ms, const Palette16 palette) {
    s->level = 0;
    s->direction = 1;
    s->random_mask = 0;
    s->color = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
    s->next_step_ms = time_ms;
}

static void cu_advance(CUStrip *s, int strip_id, double time_ms, double mod1,
                       double mod2, int num_segments, const Palette16 palette) {
    if (time_ms < s->next_step_ms) return;

    double step = cu_step_ms(strip_id, mod1, mod2);

    if (cu_global_mode == CU_MODE_RANDOM) {
        s->random_mask = (uint16_t)(rand() & 0xFFFF);
        s->next_step_ms = time_ms + step;
        return;
    }

    if (cu_global_mode == CU_MODE_SLIDE) {
        int width = cu_slide_width;
        if (width > num_segments) width = num_segments;
        int max_pos = num_segments - width;
        if (max_pos < 0) max_pos = 0;

        s->level += s->direction;
        if (s->level >= max_pos) {
            s->level = max_pos;
            s->direction = -1;
        } else if (s->level <= 0) {
            s->level = 0;
            s->direction = 1;
        }
        s->next_step_ms = time_ms + step;
        return;
    }

    // UP / DOWN share this same counting state machine; only rendering
    // interprets `level` differently depending on direction.
    if (s->level >= num_segments) {
        s->level = 0;
        s->color = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
        s->next_step_ms = time_ms + step;
    } else {
        s->level++;
        s->next_step_ms =
            time_ms + (s->level >= num_segments ? CU_HOLD_FULL_MS : step);
    }
}

static bool cu_segment_lit(const CUStrip *s, int seg, int num_segments) {
    switch (cu_global_mode) {
        case CU_MODE_UP:     return seg < s->level;
        case CU_MODE_DOWN:   return seg >= (num_segments - s->level);
        case CU_MODE_RANDOM: return (s->random_mask >> seg) & 1;
        case CU_MODE_SLIDE:  return seg >= s->level && seg < s->level + cu_slide_width;
        default:              return false;
    }
}

static void cu_render_strip(int strip_id, const CUStrip *s, int num_segments,
                            PixelFunc pixel) {
    int n = get_strip_num_leds(strip_id);
    if (n <= 0) return;

    int starts[CU_MAX_SEGMENTS], lens[CU_MAX_SEGMENTS];
    cu_compute_segments(n, num_segments, starts, lens);

    for (int i = 0; i < n; i++) {
        RGB c = {0, 0, 0};
        for (int seg = 0; seg < num_segments; seg++) {
            if (i >= starts[seg] && i < starts[seg] + lens[seg]) {
                if (cu_segment_lit(s, seg, num_segments)) c = s->color;
                break;
            }
        }
        pixel(strip_id, i, &c.r, &c.g, &c.b);
    }
}

static void cu_render_matrix(int strip_id, const CUStrip *s, int num_segments,
                             PixelFunc pixel) {
    int width = get_matrix_width(strip_id);
    int height = get_matrix_height(strip_id);
    if (width <= 0 || height <= 0) return;

    int x0[CU_MAX_SEGMENTS], x1[CU_MAX_SEGMENTS];
    int y0[CU_MAX_SEGMENTS], y1[CU_MAX_SEGMENTS];
    cu_compute_matrix_patches(width, height, num_segments, x0, x1, y0, y1);

    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            RGB c = {0, 0, 0};
            for (int p = 0; p < num_segments; p++) {
                if (x >= x0[p] && x < x1[p] && y >= y0[p] && y < y1[p]) {
                    if (cu_segment_lit(s, p, num_segments)) c = s->color;
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
        cu_state[i].direction = 1;
        cu_state[i].random_mask = 0;
        cu_state[i].next_step_ms = 0.0;
        cu_state[i].color = (RGB){0, 0, 0};
    }
    cu_shared_state.level = 0;
    cu_shared_state.direction = 1;
    cu_shared_state.random_mask = 0;
    cu_shared_state.next_step_ms = 0.0;
    cu_shared_state.color = (RGB){0, 0, 0};
    cu_lockstep = false;
    cu_slide_width = CU_SLIDE_WIDTH_MIN;
    cu_global_mode = CU_MODE_UP;
    cu_next_mode_change_ms = 0.0;
    cu_times_staggered = false;
}

// Enters a freshly-picked mode/lockstep/slide-width choice: resets either
// every strip independently (each staggered relative to `time_ms`, one
// reset per k) or the single shared state once, depending on `lockstep`.
static void cu_enter_cycle(bool lockstep, int slide_width, double time_ms,
                          const int strip_ids[], int active_count,
                          const Palette16 palette) {
    cu_lockstep = lockstep;
    cu_slide_width = slide_width;
    if (lockstep) {
        cu_reset_strip(&cu_shared_state, time_ms, palette);
    } else {
        for (int k = 0; k < active_count; k++) {
            cu_reset_strip(&cu_state[strip_ids[k]], time_ms + k * 120.0, palette);
        }
    }
}

static void count_up_update(double time_ms, PixelFunc pixel,
                            const Palette16 palette) {
    double mod1 = noise_mod1(time_ms);
    double mod2 = noise_mod2(time_ms);
    int num_segments = cu_segment_count(time_ms);

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
        cu_enter_cycle(cu_pick_lockstep(seed), cu_pick_slide_width(seed), time_ms,
                       strip_ids, active_count, palette);
        cu_times_staggered = true;
    }

    // All panels switch mode (and re-roll lockstep/slide-width) together.
    // Seed from the schedule timestamp just reached (agreed on last cycle),
    // not the current frame's time_ms -- see shared.h's deterministic_rand.
    if (time_ms >= cu_next_mode_change_ms) {
        double seed = cu_next_mode_change_ms;
        cu_global_mode = cu_pick_mode(cu_global_mode, seed);
        cu_next_mode_change_ms = seed + cu_mode_duration(seed);
        cu_enter_cycle(cu_pick_lockstep(seed), cu_pick_slide_width(seed), time_ms,
                       strip_ids, active_count, palette);
    }

    if (cu_lockstep) {
        cu_advance(&cu_shared_state, 0, time_ms, mod1, mod2, num_segments, palette);
    }

    for (int k = 0; k < active_count; k++) {
        int id = strip_ids[k];
        CUStrip *s;
        if (cu_lockstep) {
            s = &cu_shared_state;
        } else {
            cu_advance(&cu_state[id], id, time_ms, mod1, mod2, num_segments, palette);
            s = &cu_state[id];
        }
        if (id == 4) {
            cu_render_matrix(id, s, num_segments, pixel);
        } else {
            cu_render_strip(id, s, num_segments, pixel);
        }
    }
}
