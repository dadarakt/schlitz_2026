// Shared modifiers used by multiple patterns (derived from time, mirrors
// Globals.cpp updateModifiers).
#pragma once

#include <math.h>

// mod1 / mod2: sine oscillators at 0.005 and 0.008 rad per decisecond
// (original: sin(t_mod * freq) where t_mod = millis()/100)
static inline double noise_mod1(double time_ms) {
    return sin(time_ms * 0.00005);   // 0.005 / 100
}
static inline double noise_mod2(double time_ms) {
    return sin(time_ms * 0.00008);   // 0.008 / 100
}
