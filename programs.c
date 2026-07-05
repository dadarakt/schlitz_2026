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

// Matrix must stay at index 4 -- noise.c and panel_pulse.c both hardcode it.
// strip_1/strip_2 (WS2801, indices 5-6) live on the mesh node board -- see
// main.c's node LedVizConfig. .position/.length_cm for them are estimates
// (flanking the 4 bars at the outer edges) -- adjust once measured.
const StripDef strip_setup[] = {
    {.num_leds = 142, .position = -0.9f, .length_cm = 100.0f},              // bar_1 (left outer)
    {.num_leds = 142, .position = -0.3f, .length_cm = 100.0f},              // bar_2 (left inner)
    {.num_leds = 142, .position =  0.3f, .length_cm = 100.0f},              // bar_3 (right inner)
    {.num_leds = 142, .position =  0.9f, .length_cm = 100.0f},              // bar_4 (right outer)
    {.num_leds = 256, .position =  0.0f, .length_cm = 32.0f,                // matrix (center)
     .matrix_width = 32, .matrix_height = 8},
    {.num_leds = 50, .position = -1.0f, .length_cm = 35.0f},                // strip_1 (left, WS2801)
    {.num_leds = 50, .position =  1.0f, .length_cm = 35.0f},                // strip_2 (right, WS2801)
};
const int NUM_STRIPS = sizeof(strip_setup) / sizeof(strip_setup[0]);

// ============================================================================
// Patterns
// ============================================================================

#include "patterns/sync_pulse.c"
#include "patterns/noise.c"
#include "patterns/panel_pulse.c"
#include "patterns/drop.c"
#include "patterns/wave.c"
#include "patterns/searchlight.c"
#include "patterns/count_up.c"
#include "patterns/gradient_sweep.c"

// ============================================================================
// Program registry
// ============================================================================

const Program programs[] = {
    // First/default for now -- quick, obvious visual check while testing
    // mesh sync across boards (see sync_pulse.c).
    {"Sync Pulse",     sync_pulse_update,     sp_init, sp_init},
    {"Count Up",       count_up_update,       cu_init, cu_init},
    {"Gradient Sweep", gradient_sweep_update, gs_init, gs_init},
    {"Noise",          noise_update,          NULL,    NULL   },
    {"Panel Pulse",    panel_pulse_update,    pp_init, pp_init},
    {"Drop",           drop_update,           drop_init, drop_init},
    {"Wave",           wave_update,           wave_init, wave_init},
    {"Searchlight",    searchlight_update,    sl_init, sl_init},
};
const int NUM_PROGRAMS = sizeof(programs) / sizeof(programs[0]);
