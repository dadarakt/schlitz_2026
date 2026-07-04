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

// ============================================================================
// Strip setup
// ============================================================================

// DEBUG: single strip for signal integrity testing
const StripDef strip_setup[] = {
    {.num_leds = 142, .position = -0.9f, .length_cm = 100.0f},              // bar_1
};
const int NUM_STRIPS = sizeof(strip_setup) / sizeof(strip_setup[0]);

// ============================================================================
// Solid test pattern  (no frame buffers, no noise — for signal debugging)
// ============================================================================

static void solid_update(double time_ms, PixelFunc pixel,
                         const Palette16 palette) {
    // Cycle slowly through the palette (~1 full cycle per 10 seconds)
    uint8_t hue = (uint8_t)((int)(time_ms / 39.0) & 0xFF);
    RGB c = palette_sample(palette, hue, 60, false);

    int num_strips = get_num_strips();
    for (int s = 0; s < num_strips; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            pixel(s, i, &c.r, &c.g, &c.b);
        }
    }
}

// ============================================================================
// Program registry
// ============================================================================

const Program programs[] = {
    {"Solid", solid_update, NULL, NULL},
};
const int NUM_PROGRAMS = sizeof(programs) / sizeof(programs[0]);
