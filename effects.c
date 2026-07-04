// Strobe + Flash overlay effects -- ported from schlitzerei/src/StrobeEffect.cpp
// and CrowdBlinder.cpp. Both run as a single combined overlay registered via
// led_viz_set_overlay() (see led_viz_esp32.h), applied after the active
// pattern renders and before the hardware refresh -- so they work no matter
// which pattern is currently active.
//
// Strobe: continuous on/off white blink at ~15Hz while its button is held,
// overriding the active pattern completely on the bars + matrix (matches
// the original's scope -- it does not touch the WS2801 strips).
//
// Flash: a smooth fade-in/fade-out overlay while its button is held, added
// on top of whatever the active pattern is showing, across all strips.
// Unlike the original CrowdBlinder (always white), this samples a color
// from the current palette once per press and holds it for that press.

#include <led_viz_esp32.h>
#include <stdlib.h>

// -------------------- Strobe --------------------

#define STROBE_FREQUENCY_HZ 15

static volatile bool strobe_held = false;

void strobe_set_held(bool held) { strobe_held = held; }

static void strobe_render(double time_ms, PixelFunc pixel) {
    if (!strobe_held) return;

    uint32_t cycle_ms = 1000u / STROBE_FREQUENCY_HZ;
    bool on_frame = (((uint32_t)time_ms / cycle_ms) % 2u) == 0u;

    uint8_t r = on_frame ? 255 : 0;
    uint8_t g = on_frame ? 255 : 0;
    uint8_t b = on_frame ? 255 : 0;

    for (int s = 0; s <= 4; s++) { // bars + matrix only, matches original
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            pixel(s, i, &r, &g, &b);
        }
    }
}

// -------------------- Flash --------------------

#define FLASH_ATTACK_MS 200.0
#define FLASH_RELEASE_MS 400.0

// How far to push the sampled palette color toward white (0 = untouched,
// 255 = pure white). All our custom palettes are deliberately moderate
// brightness/desaturated, so adding a raw sample on top of a pattern using
// that same palette barely reads as a flash at all -- push it most of the
// way toward white for a punchy, obviously-different flash while keeping a
// hint of the palette's hue.
#define FLASH_WHITEN_AMOUNT 170

static volatile bool flash_held = false;
static bool flash_color_pending = true; // pick a fresh color on next press
static float flash_level = 0.0f;        // 0..255 overlay intensity
static double flash_last_ms = -1.0;
static RGB flash_color = {255, 255, 255};

void flash_set_held(bool held) { flash_held = held; }

static RGB blend_toward_white(RGB c, uint8_t amount) {
    c.r = qadd8(c.r, scale8((uint8_t)(255 - c.r), amount));
    c.g = qadd8(c.g, scale8((uint8_t)(255 - c.g), amount));
    c.b = qadd8(c.b, scale8((uint8_t)(255 - c.b), amount));
    return c;
}

static void flash_render(double time_ms, PixelFunc pixel,
                         GetPixelFunc get_pixel, const Palette16 palette) {
    double dt_ms = (flash_last_ms < 0.0) ? 16.0 : (time_ms - flash_last_ms);
    flash_last_ms = time_ms;

    if (flash_held) {
        // Pick one color per press (not every frame), so a single flash
        // reads as one consistent color rather than flickering hues.
        if (flash_color_pending) {
            RGB sampled = palette_sample(palette, (uint8_t)(rand() & 0xFF), 255, true);
            flash_color = blend_toward_white(sampled, FLASH_WHITEN_AMOUNT);
            flash_color_pending = false;
        }
        if (flash_level < 255.0f) {
            flash_level += (float)(255.0 * dt_ms / FLASH_ATTACK_MS);
            if (flash_level > 255.0f) flash_level = 255.0f;
        }
    } else {
        flash_color_pending = true;
        if (flash_level > 0.0f) {
            flash_level -= (float)(255.0 * dt_ms / FLASH_RELEASE_MS);
            if (flash_level < 0.0f) flash_level = 0.0f;
        }
    }

    if (flash_level <= 0.0f) return;
    uint8_t level = (uint8_t)(flash_level + 0.5f);

    for (int s = 0; s < 7; s++) {
        int n = get_strip_num_leds(s);
        for (int i = 0; i < n; i++) {
            uint8_t r = 0, g = 0, b = 0;
            get_pixel(s, i, &r, &g, &b);
            r = qadd8(r, scale8(flash_color.r, level));
            g = qadd8(g, scale8(flash_color.g, level));
            b = qadd8(b, scale8(flash_color.b, level));
            pixel(s, i, &r, &g, &b);
        }
    }
}

// -------------------- Combined overlay --------------------

// Order matches the original (strobe, then crowd-blinder/flash on top),
// so if both happen to be held at once, flash still shows through during
// strobe's black "off" frames.
void effects_overlay(double time_ms, PixelFunc pixel, GetPixelFunc get_pixel,
                     const Palette16 palette) {
    strobe_render(time_ms, pixel);
    flash_render(time_ms, pixel, get_pixel, palette);
}
