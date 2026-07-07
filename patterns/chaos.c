// Chaos pattern -- new pattern (not ported from the original schlitzerei
// codebase). Takes Count Up's segmented look further: instead of a fixed
// grid of evenly-spaced segments, each strip holds a small, variable-length
// list of moving intervals ("cells"):
//   - One cell starts small, near the middle, already drifting slightly,
//     and keeps growing wider continuously -- including after it splits or
//     merges, not just before its first split.
//   - Once a cell passes CHAOS_SPLIT_WIDTH_FRAC of the strip, it splits in
//     two -- both halves keep moving and growing, now apart from each
//     other, and will split again once *they* regrow past the threshold.
//   - Cells bounce off the strip's ends.
//   - When two cells' ranges overlap, they either merge into one (rare --
//     CHAOS_MERGE_CHANCE_PERCENT) or bounce apart (the common case). A
//     merge always picks a fresh random direction/speed for the result --
//     just averaging the two velocities let opposite-direction collisions
//     (the common case, since they had to be approaching each other to
//     collide at all) cancel out into a near-stationary cell.
//   - Any cell can also spontaneously split early, before regrowing all the
//     way to the threshold (CHAOS_RANDOM_SPLIT_CHANCE_PERCENT per decision
//     tick), which is what keeps the display getting more chaotic the
//     longer it runs, up to CHAOS_MAX_CELLS.
//   - Each cell's edges fade out over CHAOS_FEATHER_PX pixels rather than
//     cutting off hard, and a short afterglow (CHAOS_AFTERGLOW_RETAIN)
//     trails behind moving/vanishing edges instead of an instant cut.
//   - A split or merge briefly brightens toward white right at the
//     resulting cell(s)' center, fading out over the next moment
//     (CHAOS_FLASH_DECAY_PER_SEC) -- a small flourish marking the event.
//
// Movement, growth, and the flash fade are all integrated continuously
// every frame (using the elapsed real time since this strip's last frame),
// rather than only updating once per "decision tick" -- position/width
// would otherwise visibly jump once per tick instead of drifting smoothly.
// The decision tick (chaos_step_ms) still gates the less-frequent, more
// discrete events: splitting, merging, and colliding.
//
// Every measurement is a fraction of the strip's own pixel count rather
// than a fixed number of pixels, so the same feel scales from a 32-column
// matrix up to a 142-LED bar. Always runs independently per strip -- with
// strips this different in length (32 columns vs. 142 LEDs), a shared
// state wouldn't render comparably across them the way Count Up's
// lockstep does for its fixed-grid modes.

#include "shared.h"
#include <led_viz.h>
#include <stdlib.h>

#define CHAOS_MIN_STEP_MS 70.0
#define CHAOS_MAX_STEP_MS 190.0

// Cells never hard-cap out abruptly -- above CHAOS_SPLIT_THROTTLE_THRESHOLD,
// the random re-split chance (see chaos_advance) drops sharply so merges
// start to outpace new splits and the population collapses back down
// naturally, rather than freezing at a ceiling. CHAOS_MAX_CELLS is just a
// hard safety limit well above that threshold.
#define CHAOS_MAX_CELLS 5
#define CHAOS_SPLIT_THROTTLE_THRESHOLD 3
#define CHAOS_SPLIT_THROTTLE_DIVISOR 5 // random-split chance is divided by this once over threshold

#define CHAOS_MIN_WIDTH_FRAC 0.02     // cells never shrink below this fraction of the strip
// Growth stops entirely once a cell reaches this width -- during a calm
// mood phase (see below), a cell can grow well past CHAOS_SPLIT_WIDTH_FRAC
// without splitting, and without this cap it would otherwise keep
// widening indefinitely, eventually colliding with everything else on the
// strip constantly.
#define CHAOS_MAX_WIDTH_FRAC 0.10
#define CHAOS_INITIAL_WIDTH_FRAC 0.02 // the one seed cell's starting width
#define CHAOS_SPLIT_WIDTH_FRAC 0.08   // a growing cell splits once it passes this width
#define CHAOS_GROW_PER_SEC_FRAC 0.045 // width added per second, continuously, until capped -- stays slow
#define CHAOS_VEL_MAX_FRAC 0.27       // drift speed cap, in strip-fractions per second
// Chance (out of 100), per cell per decision tick, that it spontaneously
// splits even before regrowing all the way back to the threshold width --
// one contributor (along with the mood cycle below) to the display getting
// more chaotic over time rather than settling into a slow, predictable
// rhythm.
#define CHAOS_RANDOM_SPLIT_CHANCE_PERCENT 1
// Chance (out of 100), on collision, that the two cells merge into one
// instead of bouncing apart -- deliberately the rarer outcome.
#define CHAOS_MERGE_CHANCE_PERCENT 15
// Chance (out of 100), per decision tick, that a cell sitting at (or very
// near) the max-width cap simply dies off instead of continuing to sit
// there -- once a cell is capped out and can't split further (e.g. already
// at CHAOS_MAX_CELLS), it would otherwise just stay put indefinitely,
// eventually leaving the strip permanently full.
#define CHAOS_DEATH_CHANCE_PERCENT 4

// Splitting/merging isn't a constant rate -- it slowly drifts between a
// "chaotic" phase (splits fire eagerly, merges are rare) and a "calm" one
// (splits are suppressed, merges are favored), so the display settles into
// a genuinely different state every so often instead of accumulating cells
// until it's just constant flicker. Driven by the same noise_mod1/mod2
// global modifiers already used elsewhere in this codebase (searchlight.c,
// sine.c, ...) rather than a new dedicated oscillator -- see chaos_advance.

// Edge softness, in pixels -- brightness ramps from 0 to full over this
// distance from a cell's true (float) boundary, instead of a hard cut.
#define CHAOS_FEATHER_PX 2.0f

// Afterglow -- fades the previous frame's brightness by this much each
// frame (out of 255) before drawing the current cells on top, so moving or
// just-vanished edges leave a brief trail instead of an instant cut.
#define CHAOS_AFTERGLOW_RETAIN 210

// Split/merge flourish: briefly blends toward white within this radius of
// the resulting cell(s)' center, decaying continuously by this factor
// every second (e.g. 0.18 means it's down to 18% of its peak after 1s).
#define CHAOS_FLASH_RADIUS_PX 4.0f
#define CHAOS_FLASH_DECAY_PER_SEC 0.18f
#define CHAOS_FLASH_MAX_WHITE 170 // out of 255, blended in at full flash + dead center

// Indexed directly by strip id (0-3 bars, 4 matrix, 5-6 WS2801 strips)
#define CHAOS_NUM_STRIP_SLOTS 7
#define CHAOS_FB_MAX_LEDS 256

typedef struct {
    float pos;    // left edge, in pixels
    float width;  // in pixels
    float vel;    // pixels per second (can be negative)
    uint8_t hue;
    bool grown;   // true once it's split or merged at least once -- used
                  // only to gate the "respawn a fresh seed" check below,
                  // not growth or splitting themselves (both apply to
                  // every cell regardless of this flag)
    float flash;  // 0..1, decays over time -- see CHAOS_FLASH_* above
    bool dying;   // true once it's rolled a death (see chaos_advance) --
                  // frozen in place, fading out via `flash` before being
                  // actually removed once the flash is gone
} ChaosCell;

typedef struct {
    ChaosCell cells[CHAOS_MAX_CELLS];
    int num_cells;
    double next_step_ms;
    double last_update_ms; // -1 means "no previous frame yet" -- see chaos_advance
} ChaosStrip;

static ChaosStrip chaos_state[CHAOS_NUM_STRIP_SLOTS];

// Persistent per-pixel framebuffer for the afterglow trail.
static uint8_t chaos_fb_r[CHAOS_NUM_STRIP_SLOTS][CHAOS_FB_MAX_LEDS];
static uint8_t chaos_fb_g[CHAOS_NUM_STRIP_SLOTS][CHAOS_FB_MAX_LEDS];
static uint8_t chaos_fb_b[CHAOS_NUM_STRIP_SLOTS][CHAOS_FB_MAX_LEDS];

static double chaos_step_ms(int strip_id, double mod1, double mod2) {
    double m = (strip_id % 2 == 0) ? mod1 : mod2; // alternate per strip for variety
    double norm = (m + 1.0) * 0.5;                 // 0..1
    double step = CHAOS_MIN_STEP_MS + norm * (CHAOS_MAX_STEP_MS - CHAOS_MIN_STEP_MS);
    step += (double)(rand() % 61) - 30.0; // +-30ms randomness
    if (step < 30.0) step = 30.0;
    return step;
}

// The 1D pixel-space domain a strip's cells move in: the matrix uses its
// column count (cells span whole columns, full height -- see
// chaos_render_matrix), every other strip uses its actual LED count.
static int chaos_domain_size(int strip_id) {
    if (strip_id == 4) return get_matrix_width(strip_id);
    return get_strip_num_leds(strip_id);
}

// Picks a fresh random velocity, magnitude somewhere in the upper half of
// the allowed range -- used for merges (see chaos_advance) so the result
// always keeps moving with real speed, never just an average of whatever
// the two parents happened to be doing.
static float chaos_random_vel(int n) {
    float vel_max = (float)n * CHAOS_VEL_MAX_FRAC;
    float speed = vel_max * (0.5f + 0.5f * ((rand() % 100) / 100.0f));
    return (rand() % 2) ? speed : -speed;
}

// Midpoint between two palette-index hues, taking the shorter way around
// the circular 0-255 wheel -- used on merge so the result is a genuinely
// new blended color rather than just picking one of the two parents'.
static uint8_t chaos_blend_hue(uint8_t a, uint8_t b) {
    int diff = (int)b - (int)a;
    if (diff > 128) diff -= 256;
    if (diff < -128) diff += 256;
    return (uint8_t)((int)a + diff / 2);
}

static void chaos_reset_strip(ChaosStrip *cs, int n) {
    cs->num_cells = 1;
    cs->next_step_ms = 0.0;
    cs->last_update_ms = -1.0;

    ChaosCell *c = &cs->cells[0];
    c->width = (float)n * CHAOS_INITIAL_WIDTH_FRAC;
    if (c->width < 2.0f) c->width = 2.0f;
    c->pos = ((float)n - c->width) * 0.5f; // start centered
    c->hue = (uint8_t)(rand() & 0xFF);
    c->grown = false;
    c->flash = 0.0f;
    c->dying = false;

    // Small drift from the start, in a random direction -- "growing... and
    // moving slightly" happen together, not movement only after the first
    // split.
    float vel_max = (float)n * CHAOS_VEL_MAX_FRAC;
    float speed = vel_max * (0.3f + 0.5f * ((rand() % 100) / 100.0f));
    c->vel = (rand() % 2) ? speed : -speed;
}

// Splits cells[idx] in place into two half-width cells (the second
// appended as a new cell), pushing them apart so the split visibly
// separates the two halves rather than leaving them coincident. No-op if
// already at the cell cap or the cell is too narrow to halve meaningfully.
static void chaos_split(ChaosStrip *cs, int idx, int n) {
    if (cs->num_cells >= CHAOS_MAX_CELLS) return;

    float min_width = (float)n * CHAOS_MIN_WIDTH_FRAC;
    if (min_width < 1.0f) min_width = 1.0f;

    ChaosCell *c = &cs->cells[idx];
    float half = c->width * 0.5f;
    if (half < min_width) return;

    float vel_max = (float)n * CHAOS_VEL_MAX_FRAC;
    float speed = fmaxf(fabsf(c->vel), vel_max * 0.3f);
    float orig_pos = c->pos;
    uint8_t orig_hue = c->hue;

    c->width = half;
    c->vel = -speed;
    c->grown = true;
    c->flash = 1.0f;
    c->dying = false;

    ChaosCell *nc = &cs->cells[cs->num_cells++];
    nc->width = half;
    nc->pos = orig_pos + half;
    nc->vel = speed;
    // Usually inherit the parent's color (reads as one thing splitting);
    // sometimes pick a fresh one, for variety as more cells accumulate.
    nc->hue = ((rand() % 100) < 50) ? orig_hue : (uint8_t)(rand() & 0xFF);
    nc->grown = true;
    nc->flash = 1.0f;
    nc->dying = false;
}

static void chaos_advance(ChaosStrip *cs, int strip_id, double time_ms,
                          double mod1, double mod2, int n) {
    if (n <= 0) return;

    double dt_ms = (cs->last_update_ms < 0.0) ? 0.0 : time_ms - cs->last_update_ms;
    if (dt_ms < 0.0) dt_ms = 0.0;
    if (dt_ms > 100.0) dt_ms = 100.0; // clamp a stall/resume glitch to one reasonable step
    cs->last_update_ms = time_ms;
    float dt_sec = (float)(dt_ms / 1000.0);

    float vel_max = (float)n * CHAOS_VEL_MAX_FRAC;

    // 1) Continuous move/grow/flash-decay every frame -- decoupled from the
    // decision tick below, so drifting and growing read smoothly instead of
    // jumping once per (comparatively sparse) tick. Applies to every cell
    // regardless of whether it's split/merged before.
    for (int i = 0; i < cs->num_cells; i++) {
        ChaosCell *c = &cs->cells[i];
        c->flash *= powf(CHAOS_FLASH_DECAY_PER_SEC, dt_sec);
        if (c->flash < 0.02f) c->flash = 0.0f;
        // A dying cell just fades out in place via its flash decaying --
        // no movement/growth/bounce -- until chaos_advance's cleanup pass
        // below actually removes it.
        if (c->dying) continue;

        c->pos += c->vel * dt_sec;
        float max_width = (float)n * CHAOS_MAX_WIDTH_FRAC;
        if (c->width < max_width) {
            c->width += (float)n * CHAOS_GROW_PER_SEC_FRAC * dt_sec;
            if (c->width > max_width) c->width = max_width;
        }
        // Bounce off the strip's ends.
        if (c->pos < 0.0f) {
            c->pos = 0.0f;
            c->vel = fmaxf(fabsf(c->vel), vel_max * 0.3f);
        } else if (c->pos + c->width > (float)n) {
            c->pos = (float)n - c->width;
            c->vel = -fmaxf(fabsf(c->vel), vel_max * 0.3f);
        }
    }

    if (time_ms < cs->next_step_ms) return;
    cs->next_step_ms = time_ms + chaos_step_ms(strip_id, mod1, mod2);

    // Slowly oscillates between a "chaotic" phase (favors splitting,
    // disfavors merging) and a "calm" one (the reverse) -- 0 = calm,
    // 1 = chaotic. Reuses the same alternating mod1 (period ~126s) /
    // mod2 (~78.5s) global modifiers chaos_step_ms already picks between
    // by strip parity, rather than introducing a third oscillator --
    // odd/even strips naturally end up on different mood cycle lengths as
    // a result, desyncing from each other for free.
    double m = (strip_id % 2 == 0) ? mod1 : mod2;
    float mood = (float)((m + 1.0) * 0.5);

    // 2) Growth-triggered split for any cell that's (re)reached the
    // threshold width -- since growth never stops, this fires again and
    // again as each half regrows past the threshold. Not guaranteed to
    // fire the instant it crosses the threshold: during a calm phase it's
    // likely to keep growing past it for a while first, so cells can get
    // genuinely large and the pace slows down, rather than every cell
    // splitting the moment it's able to.
    float split_prob = 0.05f + 0.3f * mood;
    for (int i = 0; i < cs->num_cells && cs->num_cells < CHAOS_MAX_CELLS; i++) {
        if (!cs->cells[i].dying && cs->cells[i].width >= (float)n * CHAOS_SPLIT_WIDTH_FRAC &&
            ((rand() % 100) < (int)(split_prob * 100.0f))) {
            chaos_split(cs, i, n);
        }
    }

    // 3) Occasional spontaneous re-splits, for any cell wide enough -- on
    // top of the deterministic threshold-triggered split above, this is
    // what escalates the chaos over time. Once there are more than
    // CHAOS_SPLIT_THROTTLE_THRESHOLD cells, this chance drops sharply so
    // merges start outpacing new splits and the population collapses back
    // down on its own, rather than sitting at a hard cap.
    float min_width = (float)n * CHAOS_MIN_WIDTH_FRAC;
    int base_split_chance = (cs->num_cells > CHAOS_SPLIT_THROTTLE_THRESHOLD)
                                ? CHAOS_RANDOM_SPLIT_CHANCE_PERCENT / CHAOS_SPLIT_THROTTLE_DIVISOR
                                : CHAOS_RANDOM_SPLIT_CHANCE_PERCENT;
    int split_chance = (int)((float)base_split_chance * (0.3f + 0.7f * mood));
    for (int i = 0; i < cs->num_cells && cs->num_cells < CHAOS_MAX_CELLS; i++) {
        ChaosCell *c = &cs->cells[i];
        if (!c->dying && c->width >= 2.0f * min_width && (rand() % 100) < split_chance) {
            chaos_split(cs, i, n);
        }
    }

    // 4) Collisions -- num_cells is small (<= CHAOS_MAX_CELLS), so a plain
    // O(n^2) pass each step is cheap. Merge chance also follows the mood
    // cycle, inversely -- calm phases favor consolidating into fewer,
    // bigger cells.
    int merge_chance = (int)((float)CHAOS_MERGE_CHANCE_PERCENT * (1.5f - mood));
    if (merge_chance < 3) merge_chance = 3;
    for (int i = 0; i < cs->num_cells; i++) {
        for (int j = i + 1; j < cs->num_cells; j++) {
            ChaosCell *a = &cs->cells[i];
            ChaosCell *b = &cs->cells[j];
            if (a->dying || b->dying) continue; // a dying cell is frozen, not colliding
            bool overlap = a->pos < b->pos + b->width && b->pos < a->pos + a->width;
            if (!overlap) continue;

            if ((rand() % 100) < merge_chance) {
                float lo = fminf(a->pos, b->pos);
                float hi = fmaxf(a->pos + a->width, b->pos + b->width);
                a->pos = lo;
                a->width = hi - lo;
                // Merging can otherwise produce a cell wider than normal
                // growth would ever allow (the union of two already-large,
                // barely-overlapping cells) -- clamp to the same cap.
                float max_merge_width = (float)n * CHAOS_MAX_WIDTH_FRAC;
                if (a->width > max_merge_width) a->width = max_merge_width;
                // Fresh direction/speed, not an average -- see the file
                // comment on why averaging left merged cells stationary.
                a->vel = chaos_random_vel(n);
                a->hue = chaos_blend_hue(a->hue, b->hue);
                a->grown = true;
                a->flash = 1.0f;
                for (int k = j; k < cs->num_cells - 1; k++) cs->cells[k] = cs->cells[k + 1];
                cs->num_cells--;
                j--; // re-check the slot that just shifted into j
            } else {
                // Bounce: whichever cell is more to the left gets pushed
                // further left, the other further right -- guarantees they
                // separate next step regardless of their prior velocities.
                bool a_is_left = (a->pos + a->width * 0.5f) < (b->pos + b->width * 0.5f);
                ChaosCell *left = a_is_left ? a : b;
                ChaosCell *right = a_is_left ? b : a;
                left->vel = -fmaxf(fabsf(left->vel), vel_max * 0.3f);
                right->vel = fmaxf(fabsf(right->vel), vel_max * 0.3f);

                float overlap_amt = fminf(left->pos + left->width, right->pos + right->width) -
                                    fmaxf(left->pos, right->pos);
                if (overlap_amt > 0.0f) {
                    left->pos -= overlap_amt * 0.5f + 0.5f;
                    right->pos += overlap_amt * 0.5f + 0.5f;
                }
            }
        }
    }

    // Safety clamp -- collision nudges above could in principle push a
    // cell just past an end.
    for (int i = 0; i < cs->num_cells; i++) {
        ChaosCell *c = &cs->cells[i];
        if (c->width > (float)n) c->width = (float)n;
        if (c->pos < 0.0f) c->pos = 0.0f;
        if (c->pos + c->width > (float)n) c->pos = (float)n - c->width;
    }

    // A cell sitting at (or very near) the max-width cap gets a small
    // chance to start dying each tick, rather than sitting there
    // indefinitely once it can no longer split (e.g. already at
    // CHAOS_MAX_CELLS) -- keeps the strip from filling up wall-to-wall and
    // staying that way. Marked `dying` rather than removed outright so it
    // gets a moment to glow and fade in place first (frozen -- see the
    // continuous move/grow loop above) instead of an instant cut.
    float max_width = (float)n * CHAOS_MAX_WIDTH_FRAC;
    for (int i = 0; i < cs->num_cells; i++) {
        ChaosCell *c = &cs->cells[i];
        if (!c->dying && c->width >= max_width * 0.95f &&
            (rand() % 100) < CHAOS_DEATH_CHANCE_PERCENT) {
            c->dying = true;
            c->flash = 1.0f;
        }
    }

    // Actually remove cells whose death-glow has fully faded.
    for (int i = 0; i < cs->num_cells; i++) {
        if (cs->cells[i].dying && cs->cells[i].flash <= 0.0f) {
            for (int k = i; k < cs->num_cells - 1; k++) cs->cells[k] = cs->cells[k + 1];
            cs->num_cells--;
            i--;
        }
    }

    // Enough merges/deaths can converge everything down to zero or one
    // cell, at which point there's no more chaos left to escalate --
    // respawn a fresh seed so the whole grow/split/collide cycle starts
    // over. The single-cell case is gated on `grown` so this doesn't fire
    // on a brand new strip's still-growing first cell, before it's ever
    // had the chance to split at all, and excludes a still-fading dying
    // cell so its glow isn't cut short by an immediate respawn.
    if (cs->num_cells == 0 ||
        (cs->num_cells == 1 && cs->cells[0].grown && !cs->cells[0].dying)) {
        chaos_reset_strip(cs, n);
    }
}

// Color at pixel-space position p: picks whichever cell covers p most (by
// distance to its nearest edge, feathered), samples its color at that
// coverage-derived brightness, then blends toward white near its center if
// it's mid-flash from a recent split/merge.
static RGB chaos_sample_at(const ChaosStrip *cs, float p, const Palette16 palette) {
    uint8_t best_cov = 0;
    const ChaosCell *best = NULL;
    for (int k = 0; k < cs->num_cells; k++) {
        const ChaosCell *cell = &cs->cells[k];
        float left = p - cell->pos;
        float right = (cell->pos + cell->width) - p;
        float d = fminf(left, right);
        if (d <= 0.0f) continue;
        float f = d / CHAOS_FEATHER_PX;
        if (f > 1.0f) f = 1.0f;
        uint8_t cov = (uint8_t)(f * 255.0f + 0.5f);
        if (cov > best_cov) {
            best_cov = cov;
            best = cell;
        }
    }
    if (!best || best_cov == 0) return (RGB){0, 0, 0};

    float center = best->pos + best->width * 0.5f;
    RGB c = palette_sample(palette, best->hue, best_cov, true);
    if (best->flash > 0.0f) {
        float dist = fabsf(p - center);
        float glow = best->flash * fmaxf(0.0f, 1.0f - dist / CHAOS_FLASH_RADIUS_PX);
        if (glow > 0.0f) {
            uint8_t white_amt = (uint8_t)(glow * CHAOS_FLASH_MAX_WHITE);
            c.r = qadd8(c.r, white_amt);
            c.g = qadd8(c.g, white_amt);
            c.b = qadd8(c.b, white_amt);
        }
    }
    return c;
}

// Fades the framebuffer pixel by CHAOS_AFTERGLOW_RETAIN, then raises it to
// the freshly-sampled color if that's brighter on a given channel -- not
// an additive blend, so overlapping/trailing colors never wash toward
// white the way summing different hues would (see noise.c's fb_desaturate
// for the same concern in a different pattern).
static void chaos_blend_afterglow(uint8_t *r, uint8_t *g, uint8_t *b, RGB fresh) {
    *r = scale8(*r, CHAOS_AFTERGLOW_RETAIN);
    *g = scale8(*g, CHAOS_AFTERGLOW_RETAIN);
    *b = scale8(*b, CHAOS_AFTERGLOW_RETAIN);
    if (fresh.r > *r) *r = fresh.r;
    if (fresh.g > *g) *g = fresh.g;
    if (fresh.b > *b) *b = fresh.b;
}

static void chaos_render_1d(int strip_id, const ChaosStrip *cs, PixelFunc pixel,
                            const Palette16 palette) {
    int n = get_strip_num_leds(strip_id);
    if (n <= 0) return;
    if (n > CHAOS_FB_MAX_LEDS) n = CHAOS_FB_MAX_LEDS;

    for (int i = 0; i < n; i++) {
        RGB c = chaos_sample_at(cs, (float)i + 0.5f, palette);
        chaos_blend_afterglow(&chaos_fb_r[strip_id][i], &chaos_fb_g[strip_id][i],
                              &chaos_fb_b[strip_id][i], c);
        pixel(strip_id, i, &chaos_fb_r[strip_id][i], &chaos_fb_g[strip_id][i],
              &chaos_fb_b[strip_id][i]);
    }
}

// Cells span whole columns (matching chaos_domain_size's use of matrix
// width as the 1D domain), lit for the full column height -- simpler than
// a row/column patch grid, and reads fine for a beam-like chaotic sweep
// across the matrix.
static void chaos_render_matrix(int strip_id, const ChaosStrip *cs, PixelFunc pixel,
                                const Palette16 palette) {
    int width = get_matrix_width(strip_id);
    int height = get_matrix_height(strip_id);
    if (width <= 0 || height <= 0) return;

    for (int x = 0; x < width; x++) {
        RGB c = chaos_sample_at(cs, (float)x + 0.5f, palette);
        for (int y = 0; y < height; y++) {
            int led = get_matrix_index(strip_id, x, y);
            if (led < 0 || led >= CHAOS_FB_MAX_LEDS) continue;
            chaos_blend_afterglow(&chaos_fb_r[strip_id][led], &chaos_fb_g[strip_id][led],
                                  &chaos_fb_b[strip_id][led], c);
            pixel(strip_id, led, &chaos_fb_r[strip_id][led], &chaos_fb_g[strip_id][led],
                  &chaos_fb_b[strip_id][led]);
        }
    }
}

static void chaos_init(void) {
    static const int strip_ids[] = {0, 1, 2, 3, 4, 5, 6};
    for (int k = 0; k < CHAOS_NUM_STRIP_SLOTS; k++) {
        int id = strip_ids[k];
        chaos_reset_strip(&chaos_state[id], chaos_domain_size(id));
        for (int i = 0; i < CHAOS_FB_MAX_LEDS; i++) {
            chaos_fb_r[id][i] = 0;
            chaos_fb_g[id][i] = 0;
            chaos_fb_b[id][i] = 0;
        }
    }
}

static void chaos_update(double time_ms, PixelFunc pixel, const Palette16 palette) {
    double mod1 = noise_mod1(time_ms);
    double mod2 = noise_mod2(time_ms);

    static const int strip_ids[] = {0, 1, 2, 3, 4, 5, 6};
    const int active_count = (int)(sizeof(strip_ids) / sizeof(strip_ids[0]));

    for (int k = 0; k < active_count; k++) {
        int id = strip_ids[k];
        int n = chaos_domain_size(id);
        chaos_advance(&chaos_state[id], id, time_ms, mod1, mod2, n);
        if (id == 4) {
            chaos_render_matrix(id, &chaos_state[id], pixel, palette);
        } else {
            chaos_render_1d(id, &chaos_state[id], pixel, palette);
        }
    }
}
