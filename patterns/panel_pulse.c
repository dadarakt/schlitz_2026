// Panel Pulse pattern -- ported from schlitzerei/src/patterns/PanelPulse.cpp

#include "shared.h"
#include <led_viz.h>
#include <math.h>
#include <stdlib.h>

// Configurable envelope + density parameters
#define PP_ATTACK_MS  120
#define PP_DECAY_MS   700
#define PP_MIN_HPM     20   // hits per minute at mod minimum
#define PP_MAX_HPM     40   // hits per minute at mod maximum

// One state entry per strip (BAR1-4, MATRIX, STRIP1, STRIP2)
#define PP_NUM_PANELS  7

typedef struct {
    uint8_t  level;           // envelope 0..255
    bool     rising;          // true = attack, false = decay
    RGB      color;
    uint32_t next_event_at;   // ms timestamp
    uint32_t last_ms;         // ms of previous frame (0 = uninitialised)
} PPPanel;

static PPPanel pp[PP_NUM_PANELS];

// --- helpers -----------------------------------------------------------------

static float pp_clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static float pp_norm(double m)   { return pp_clamp01((float)((m + 1.0) * 0.5)); }
static float pp_lerpf(float a, float b, float t) { return a + (b - a) * t; }

static uint8_t pp_density(int id, double mod1, double mod2) {
    float t;
    if (id < 4)      t = pp_norm(mod1);                               // bars
    else if (id == 4) t = pp_norm(mod2);                              // matrix
    else             t = 0.5f * (pp_norm(mod1) + pp_norm(mod2));     // strips
    return (uint8_t)(pp_lerpf(PP_MIN_HPM, PP_MAX_HPM, t) + 0.5f);
}

static uint32_t pp_next_event(uint32_t t_now, uint8_t hpm) {
    if (hpm == 0) return t_now + 0x7FFFFFFFu;
    uint32_t mean_ms = 60000UL / hpm;
    float k = -logf(((uint8_t)(rand() & 0xFF) + 1) / 256.0f);
    return t_now + (uint32_t)(mean_ms * k);
}

static void pp_step(PPPanel *p, uint32_t dt) {
    if (p->rising) {
        uint32_t inc = 255u * dt / (PP_ATTACK_MS ? PP_ATTACK_MS : 1);
        uint16_t tmp = p->level + (uint16_t)(inc > 255 ? 255 : inc);
        p->level  = tmp >= 255 ? 255 : (uint8_t)tmp;
        if (p->level == 255) p->rising = false;
    } else {
        uint32_t dec = 255u * dt / (PP_DECAY_MS ? PP_DECAY_MS : 1);
        p->level = qsub8(p->level, (uint8_t)(dec > 255 ? 255 : dec));
    }
}

static void pp_maybe_fire(PPPanel *p, int id, uint32_t t_now,
                           double mod1, double mod2, const Palette16 palette) {
    // Retrigger even if still mid-decay (matches original: level < 32, not
    // just fully dark) so flashes can overlap instead of gating on a full
    // fade-to-black -- otherwise the pattern is noticeably sparser than
    // intended at the high end of the density range.
    if (t_now >= p->next_event_at && p->level < 32) {
        p->rising = true;
        p->color  = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
        // Floor so a new flash doesn't ramp up from a low decaying value.
        if (p->level < 16) p->level = 16;
        p->next_event_at = pp_next_event(t_now, pp_density(id, mod1, mod2));
    }
}

static RGB pp_scale(RGB c, uint8_t lv) {
    return (RGB){scale8(c.r, lv), scale8(c.g, lv), scale8(c.b, lv)};
}

static void pp_draw(int s, PPPanel *p, PixelFunc pixel) {
    int n = get_strip_num_leds(s);

    if (s == 5 || s == 6) {
        // WS2801 strips: grow/shrink fill at constant brightness
        int n_on = (int)(p->level / 255.0f * n + 0.5f);
        if (n_on > n) n_on = n;
        RGB c = p->color;
        RGB black = {0, 0, 0};
        for (int i = 0; i < n_on; i++) pixel(s, i, &c.r, &c.g, &c.b);
        for (int i = n_on; i < n;   i++) pixel(s, i, &black.r, &black.g, &black.b);
    } else {
        // Bars / matrix: full fill with level-faded color
        RGB c = pp_scale(p->color, p->level);
        for (int i = 0; i < n; i++) pixel(s, i, &c.r, &c.g, &c.b);
    }
}

static void pp_init(void) {
    // Reset panel state so re-entering the pattern starts clean.
    // Stagger first-fire times so all panels don't flash simultaneously on start.
    for (int i = 0; i < PP_NUM_PANELS; i++) {
        pp[i].level         = 0;
        pp[i].rising        = false;
        pp[i].last_ms       = 0;
        // Spread first events ~0-2 seconds apart
        pp[i].next_event_at = (uint32_t)(i * 300);
    }
}

static void panel_pulse_update(double time_ms, PixelFunc pixel,
                                const Palette16 palette) {
    double mod1 = noise_mod1(time_ms);
    double mod2 = noise_mod2(time_ms);
    uint32_t t_now = (uint32_t)time_ms;

    // Clear all strips
    RGB black = {0, 0, 0};
    for (int s = 0; s < PP_NUM_PANELS; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) pixel(s, i, &black.r, &black.g, &black.b);
    }

    for (int id = 0; id < PP_NUM_PANELS; id++) {
        PPPanel *p = &pp[id];
        uint32_t dt = (p->last_ms == 0) ? 0 : (t_now - p->last_ms);
        p->last_ms = t_now;

        pp_maybe_fire(p, id, t_now, mod1, mod2, palette);
        pp_step(p, dt);

        if (p->level > 0) pp_draw(id, p, pixel);
    }
}
