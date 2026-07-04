// Schlitzerei LED programs for led_viz
// Usage: led_viz ./schlitzerei_programs.c
//
// Strip layout mirrors the physical schlitzerei hardware:
//   0  bar_1   – left outer  (142 LEDs)
//   1  bar_2   – left inner  (142 LEDs)
//   2  bar_3   – right inner (142 LEDs)
//   3  bar_4   – right outer (142 LEDs)
//   4  matrix  – center 32×8 (256 LEDs)
//   5  strip_1 – left  strip  (50 LEDs)
//   6  strip_2 – right strip  (50 LEDs)

#include <led_viz.h>
#include <stdlib.h>

// ============================================================================
// Strip setup
// ============================================================================

const StripDef strip_setup[] = {
    {.num_leds = 142, .position = -0.9f,  .length_cm = 100.0f},              // bar_1
    {.num_leds = 142, .position = -0.55f, .length_cm = 100.0f},              // bar_2
    {.num_leds = 142, .position =  0.55f, .length_cm = 100.0f},              // bar_3
    {.num_leds = 142, .position =  0.9f,  .length_cm = 100.0f},              // bar_4
    {.num_leds = 256, .position =  0.0f,  .length_cm =  32.0f,               // matrix
     .matrix_width = 32, .matrix_height = 8},
    {.num_leds =  50, .position = -0.25f, .length_cm =  35.0f},              // strip_1
    {.num_leds =  50, .position =  0.25f, .length_cm =  35.0f},              // strip_2
};
const int NUM_STRIPS = sizeof(strip_setup) / sizeof(strip_setup[0]);

// ============================================================================
// Shared modifiers  (derived from time, mirrors Globals.cpp updateModifiers)
// ============================================================================

// mod1 / mod2: sine oscillators at 0.005 and 0.008 rad per decisecond
// (original: sin(t_mod * freq) where t_mod = millis()/100)
static double noise_mod1(double time_ms) {
    return sin(time_ms * 0.00005);   // 0.005 / 100
}
static double noise_mod2(double time_ms) {
    return sin(time_ms * 0.00008);   // 0.008 / 100
}

// ============================================================================
// Noise pattern
// ============================================================================

#define NOISE_DIM   32   // MATRIX_MAX_DIMENSION = max(32, 8)
#define NUM_BARS     4
#define NUM_WSTRIPS  2

// Per-frame fade amount (matches decay_rate = 100 in Globals.cpp;
// FastLED fadeToBlackBy(100) scales each channel by (255-100)/255)
#define FADE_SCALE  155  // (255 - 100) == 155

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

// ============================================================================
// Panel Pulse pattern
// ============================================================================

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
    if (t_now >= p->next_event_at && p->level < 32) {
        p->rising = true;
        // Only pick a new color when fully dark to avoid mid-decay color flashes
        if (p->level == 0) {
            p->color = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
        }
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

// ============================================================================
// Program registry
// ============================================================================

const Program programs[] = {
    {"Noise",       noise_update,       NULL,    NULL   },
    {"Panel Pulse", panel_pulse_update, pp_init, pp_init},
};
const int NUM_PROGRAMS = sizeof(programs) / sizeof(programs[0]);
