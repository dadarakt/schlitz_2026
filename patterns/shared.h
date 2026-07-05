// Shared modifiers used by multiple patterns (derived from time, mirrors
// Globals.cpp updateModifiers).
#pragma once

#include <math.h>
#include <stdint.h>

// mod1 / mod2: sine oscillators at 0.005 and 0.008 rad per decisecond
// (original: sin(t_mod * freq) where t_mod = millis()/100)
static inline double noise_mod1(double time_ms) {
    return sin(time_ms * 0.00005);   // 0.005 / 100
}
static inline double noise_mod2(double time_ms) {
    return sin(time_ms * 0.00008);   // 0.008 / 100
}

// Deterministic pseudo-random value derived purely from a timestamp (a
// splitmix64-style finalizer) -- lets multi-board setups synced via
// led_viz_set_clock_offset_us make identical "random" choices
// independently, with zero extra network messages.
//
// Always seed from an already-*scheduled* timestamp that both boards
// separately compute via the same deterministic formula chain (e.g. "the
// mode-change time we all agreed on last cycle"), never from "now": each
// board's frame loop polls independently, so even with clocks synced,
// "now" can land one board a few ms into the next quantization bucket
// while the other is still in the previous one, silently forking the
// whole chain from that point on. `salt` distinguishes multiple
// independent-looking values derived from the same seed (e.g. one call
// for "which mode", another for "how long").
static inline uint32_t deterministic_rand(double seed_ms, uint32_t salt) {
    uint64_t x = (uint64_t)(seed_ms * 1000.0); // microsecond-resolution int
    x ^= (uint64_t)salt * 0x9E3779B97F4A7C15ULL;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return (uint32_t)x;
}

// Rounds time_ms down to a coarse granularity so two boards that reach the
// "same" logical scheduling moment a few milliseconds apart (network
// propagation delay, independent per-board frame polling) land on an
// identical value to seed deterministic_rand with. Only needed to start a
// deterministic chain from real elapsed time (e.g. right after switching
// to a pattern) -- every decision after that seeds from a value already
// agreed via the chain itself.
static inline double quantize_sync_ms(double time_ms, double granularity_ms) {
    return floor(time_ms / granularity_ms) * granularity_ms;
}
