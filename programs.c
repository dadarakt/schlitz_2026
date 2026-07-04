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
// Patterns
// ============================================================================

#include "patterns/noise.c"
#include "patterns/panel_pulse.c"

// ============================================================================
// Program registry
// ============================================================================

const Program programs[] = {
    {"Noise",       noise_update,       NULL,    NULL   },
    {"Panel Pulse", panel_pulse_update, pp_init, pp_init},
};
const int NUM_PROGRAMS = sizeof(programs) / sizeof(programs[0]);
