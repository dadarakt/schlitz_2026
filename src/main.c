#include "led_viz_esp32.h"
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>

// ets_delay_us is always available as an ESP32 ROM function
extern void ets_delay_us(unsigned int us);

#include "../effects.c"
#include "../programs.c"

#include "mesh/role.h"
#ifdef MESH_ENABLED
#if !defined(IS_ROOT)
#error "MESH_ENABLED requires ROLE_ROOT or ROLE_NODE to also be defined"
#endif
#include "mesh/mesh_common.h"
// Both mesh_root.c and mesh_node.c are always compiled and linked into
// every build regardless of role (see src/CMakeLists.txt's blanket
// src/*.* glob) -- mesh_common.c's espnow_init() picks which one to
// actually start at runtime based on the is_root argument passed to
// mesh_init() below. Both headers are therefore needed here unconditionally
// so their (always-present) symbols type-check, even though only one path
// is ever exercised on a given board.
#include "mesh/mesh_node.h"
#include "mesh/mesh_root.h"
#endif

static const char *TAG = "schlitzerei";

// ============================================================================
// Palette list
// ============================================================================

static const Palette16 *palettes[] = {
    &PALETTE_LAVA, &PALETTE_OCEAN, &PALETTE_FOREST, &PALETTE_PARTY,
    &PALETTE_HEAT, &PALETTE_ROSE,  &PALETTE_SUNSET,
};
#define NUM_PALETTES ((int)(sizeof(palettes) / sizeof(palettes[0])))

// advance_program/advance_palette (and the state they track) are root-only
// -- root is the only board with buttons to trigger them, and the only one
// that needs to track "current" program/palette at all (the node just
// mirrors whatever mesh_node_apply_state is told).
#if !defined(MESH_ENABLED) || IS_ROOT

static int current_program_idx = 0;
static int current_palette_idx = 0;

static void advance_program(void) {
  current_program_idx = (current_program_idx + 1) % NUM_PROGRAMS;
  led_viz_set_program(current_program_idx);
  ESP_LOGI(TAG, "Program → %d", current_program_idx);
}

static void advance_palette(void) {
  current_palette_idx = (current_palette_idx + 1) % NUM_PALETTES;
  led_viz_set_palette(palettes[current_palette_idx]);
  ESP_LOGI(TAG, "Palette → %d", current_palette_idx);
}

#endif // !defined(MESH_ENABLED) || IS_ROOT

// ============================================================================
// Mesh sync: node-side state application
//
// mesh_node.c calls this (declared in mesh/mesh_node.h) whenever this
// board's displayed program/palette/brightness should change -- at boot
// before ever hearing from root, whenever a state broadcast arrives, and
// whenever root is presumed lost (re-applied with the same values, just
// connected=false). See mesh_node.h for the full contract.
//
// Defined unconditionally whenever MESH_ENABLED is set (both root and node
// builds), since mesh_node.c is always compiled and linked in regardless of
// role -- see the include comment above. In practice this only ever runs
// on a node board: mesh_common.c's espnow_init() only calls
// mesh_node_start() (and hence ever reaches this function) when is_root is
// false, so it's simply dead code on a root board, never invoked.
//
// `connected` isn't used for dimming here anymore -- the disconnected case
// now has a dedicated, unmissable indicator instead of a soft brightness
// cue: see mesh_node_offline_overlay (mesh/mesh_node.c), which fully
// overrides the display with a faint red glimmer while disconnected, and
// is composed into the node's overlay below. Kept in the signature since
// mesh_node.c still needs to tell us when a re-apply is just the
// disconnect case (same program/palette/brightness as before) vs a real
// state change.
// ============================================================================

#ifdef MESH_ENABLED
void mesh_node_apply_state(uint8_t program_idx, uint8_t palette_idx,
                           uint8_t brightness, bool connected) {
  (void)connected;
  if (program_idx < NUM_PROGRAMS) {
    led_viz_set_program(program_idx);
  }
  if (palette_idx < NUM_PALETTES) {
    led_viz_set_palette(palettes[palette_idx]);
  }
  led_viz_set_brightness(brightness);
}

#if !IS_ROOT
// Node builds compose the normal effects overlay (strobe/flash -- currently
// always a no-op here, since those buttons only exist on root, but kept in
// case they're ever synced to the node too) with the disconnected-state
// glimmer, which runs after and fully overrides the display when this node
// has no connection to root.
static void node_overlay(double time_ms, PixelFunc pixel, GetPixelFunc get_pixel,
                         const Palette16 palette) {
  effects_overlay(time_ms, pixel, get_pixel, palette);
  mesh_node_offline_overlay(time_ms, pixel, get_pixel, palette);
}
#endif
#endif // MESH_ENABLED

// ============================================================================
// Automode  (mirrors autoCyclePatterns/autoCyclePalettes in main.cpp, toggled
// together by the button 0+1 combo) -- root only, only triggered from
// mux_update()'s button handling below.
// ============================================================================

#if !defined(MESH_ENABLED) || IS_ROOT

#define AUTO_CYCLE_PATTERN_MS (177 * 1000)
#define AUTO_CYCLE_PALETTE_MS (111 * 1000)

static bool auto_cycle_patterns = false;
static bool auto_cycle_palettes = false;
static double auto_next_pattern_ms = 0.0;
static double auto_next_palette_ms = 0.0;

static void auto_mode_toggle(double now_ms) {
  bool enable = !auto_cycle_patterns; // both flags always move together
  auto_cycle_patterns = enable;
  auto_cycle_palettes = enable;
  auto_next_pattern_ms = now_ms + AUTO_CYCLE_PATTERN_MS;
  auto_next_palette_ms = now_ms + AUTO_CYCLE_PALETTE_MS;
  ESP_LOGI(TAG, "Automode → %s", enable ? "on" : "off");
}

static void auto_mode_update(double now_ms) {
  if (auto_cycle_patterns && now_ms >= auto_next_pattern_ms) {
    advance_program();
    auto_next_pattern_ms = now_ms + AUTO_CYCLE_PATTERN_MS;
  }
  if (auto_cycle_palettes && now_ms >= auto_next_palette_ms) {
    advance_palette();
    auto_next_palette_ms = now_ms + AUTO_CYCLE_PALETTE_MS;
  }
}

#endif // !defined(MESH_ENABLED) || IS_ROOT

// ============================================================================
// Mux configuration + reading + button/pot handling  (mirrors MuxCfg /
// MuxInput.cpp) -- root only. The node board has no physical controls of
// its own; its program/palette/brightness are entirely driven by whatever
// the root broadcasts (see mesh_node_apply_state above).
// ============================================================================

#if !defined(MESH_ENABLED) || IS_ROOT

#define MUX_SIG_GPIO 35
#define MUX_ADC_UNIT ADC_UNIT_1
#define MUX_ADC_CH ADC_CHANNEL_7 // GPIO35 = ADC1 ch7
#define S0_GPIO 25
#define S1_GPIO 26
#define S2_GPIO 27

// Mux channel assignments
#define BTN0_CH 2
#define BTN1_CH 1
#define BTN2_CH 0
#define BTN3_CH 3
#define POT_CH 4

#define PRESS_THRESHOLD 1000 // ADC raw < threshold → pressed (pull-up)

// Brightness range (0–255).  Pot at minimum → BRI_MIN, pot at maximum →
// BRI_MAX.
#define BRI_MIN 0
#define BRI_MAX 100

static void mux_select(uint8_t ch) {
  gpio_set_level(S0_GPIO, ch & 0x01);
  gpio_set_level(S1_GPIO, (ch >> 1) & 0x01);
  gpio_set_level(S2_GPIO, (ch >> 2) & 0x01);
  esp_rom_delay_us(10); // mux settle
}

static adc_oneshot_unit_handle_t s_adc_handle;

static int mux_read_ch(uint8_t ch) {
  mux_select(ch);
  int raw = 0;
  adc_oneshot_read(s_adc_handle, MUX_ADC_CH,
                   &raw); // dummy read to charge S/H cap
  ets_delay_us(40);
  adc_oneshot_read(s_adc_handle, MUX_ADC_CH, &raw);
  return raw;
}

static void mux_init(void) {
  gpio_config_t io = {
      .pin_bit_mask = (1ULL << S0_GPIO) | (1ULL << S1_GPIO) | (1ULL << S2_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);

  adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = MUX_ADC_UNIT};
  adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);

  adc_oneshot_chan_cfg_t chan_cfg = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_12,
  };
  adc_oneshot_config_channel(s_adc_handle, MUX_ADC_CH, &chan_cfg);
}

static void mux_update(void) {
  static bool last_btn[4] = {false, false, false, false};
  static bool combo_active = false;

  double now_ms = esp_timer_get_time() / 1000.0;

  // --- Read raw values ---
  bool btn[4];
  for (int i = 0; i < 4; i++) {
    static const uint8_t ch[4] = {BTN0_CH, BTN1_CH, BTN2_CH, BTN3_CH};
    btn[i] = (mux_read_ch(ch[i]) < PRESS_THRESHOLD);
  }
  int pot_raw = mux_read_ch(POT_CH);

  // --- Pot → brightness: raw 0 → BRI_MAX, raw 4095 → BRI_MIN ---
  int bri = BRI_MIN + ((4095 - pot_raw) * (BRI_MAX - BRI_MIN)) / 4095;
  led_viz_set_brightness((uint8_t)bri);

  // --- Buttons 0 & 1: trigger on RELEASE, detect combo ---
  if (btn[0] && btn[1])
    combo_active = true;

  // Button 0 released
  if (!btn[0] && last_btn[0]) {
    if (combo_active && !btn[1]) {
      // both were held together, now both released → toggle automode
      auto_mode_toggle(now_ms);
      combo_active = false;
    } else if (!combo_active) {
      advance_program();
    }
  }

  // Button 1 released
  if (!btn[1] && last_btn[1]) {
    if (!combo_active) {
      advance_palette();
    }
  }

  // --- Button 2: Flash (while held) ---
  if (btn[2] != last_btn[2]) {
    flash_set_held(btn[2]);
  }

  // --- Button 3: Strobe (while held) ---
  if (btn[3] != last_btn[3]) {
    strobe_set_held(btn[3]);
  }

  for (int i = 0; i < 4; i++)
    last_btn[i] = btn[i];

  auto_mode_update(now_ms);

#if defined(MESH_ENABLED) && IS_ROOT
  // Keep the other board's mesh_root_notify_state up to date -- cheap to
  // call every tick, it only actually broadcasts on change (or its own
  // periodic resend timer). Flash rides along too, so the node mirrors
  // root's exact flash color instead of picking its own.
  RGB flash_color = flash_get_color();
  mesh_root_notify_state((uint8_t)current_program_idx,
                         (uint8_t)current_palette_idx, (uint8_t)bri,
                         flash_is_held(), flash_color.r, flash_color.g,
                         flash_color.b);
#endif
}

#endif // !defined(MESH_ENABLED) || IS_ROOT

// ============================================================================
// LED task
// ============================================================================

static void led_task(void *arg) {
  led_viz_run(); // blocking
  vTaskDelete(NULL);
}

// ============================================================================
// Entry point
// ============================================================================

void app_main(void) {
  ESP_LOGI(TAG, "Schlitzerei starting");

#if !defined(MESH_ENABLED) || IS_ROOT
  mux_init();
#endif

  // Cap concurrent RMT refresh to reduce refill-interrupt contention.
  // 1 = fully sequential (slowest, guaranteed no collisions -- the
  // pre-parallelization behavior). 5 = fully parallel (fastest, but
  // showed occasional flicker with all 5 channels active at once).
  // Retune based on what you see on hardware.
#if !defined(MESH_ENABLED)
  // Default single-board build: all 4 bars + matrix on one board, WS2801
  // strips not yet wired up.
  //   0  bar_1  → GPIO 22   1  bar_2  → GPIO 23
  //   2  bar_3  → GPIO 4    3  bar_4  → GPIO 15
  //   4  matrix → GPIO 2
  LedVizConfig config = {
      .gpio_pins = {22, 23, 4, 15, 2},
      .target_fps = 60,
      .max_concurrent_refresh = 2,
  };
#elif IS_ROOT
  // Mesh-sync bench test: root keeps the matrix + the two inner bars
  // (bar2, bar3). The two outer bars (bar1, bar4) are physically
  // separated onto the node board instead -- 0 means "no hardware here",
  // led_viz_init skips creating an RMT device for that strip index (see
  // led_viz_esp32.c's gpio_pin==0 handling) rather than treating it as an
  // error.
  // Matrix moved off GPIO 2 to GPIO 15 -- GPIO 2 is an ESP32 strapping pin
  // (sampled at boot for flash-mode selection), a plausible contributor to
  // the matrix-only white-pixel flicker (bars, on ordinary GPIOs, never
  // showed it). GPIO 15 is free here now that bar4 moved to the node
  // board (was bar4's pin back when this board drove all 5 strips).
  //   0  bar_1  → (node)    1  bar_2  → GPIO 23
  //   2  bar_3  → GPIO 4    3  bar_4  → (node)
  //   4  matrix → GPIO 15
  LedVizConfig config = {
      .gpio_pins = {0, 23, 4, 0, 15},
      .target_fps = 60,
      .max_concurrent_refresh = 2,
  };
#else
  // Mesh-sync bench test: node drives just the two outer bars (bar1,
  // bar4) -- see the root branch above for the full split. Pins 25/26
  // (not 22/15) per physical wiring constraints -- needed to land on the
  // same side as the 5V input pin. Safe to reuse here: the mux code
  // (which normally claims GPIO 25/26 as S0/S1 on root) is compiled out
  // entirely for the node build.
  //   0  bar_1  → GPIO 25   1  bar_2  → (root)
  //   2  bar_3  → (root)    3  bar_4  → GPIO 26
  //   4  matrix → (root)
  LedVizConfig config = {
      .gpio_pins = {25, 0, 0, 26, 0},
      .target_fps = 60,
      .max_concurrent_refresh = 2,
  };
#endif

  if (led_viz_init(&config) != 0) {
    ESP_LOGE(TAG, "LED init failed");
    return;
  }

  led_viz_set_program(0);
  led_viz_set_palette(palettes[0]);
#if defined(MESH_ENABLED) && !IS_ROOT
  led_viz_set_overlay(node_overlay);
#else
  led_viz_set_overlay(effects_overlay);
#endif
  led_viz_set_brightness(0); // start dark for fade-in

  // Run LED loop in its own task so app_main can poll the mux (or, on a
  // node board, just idle).
  // Pinned to core 1: app_main (and its ADC-based mux polling, root/default
  // builds only) is pinned to core 0
  // (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0), and classic ESP32's RMT has no
  // DMA -- it refills its small hardware buffer via interrupt many times
  // per frame. Without an explicit affinity here, the scheduler could place
  // this task on core 0 too, and an ADC read (or, on a node board, WiFi/
  // ESP-NOW handling) delaying that refill interrupt corrupts whatever bits
  // were mid-flight, splicing a wrong color into a few LEDs. Keeping it off
  // core 0 entirely avoids the contention.
  xTaskCreatePinnedToCore(led_task, "led", 8192, NULL, 5, NULL, 1);

#ifdef MESH_ENABLED
  mesh_init(IS_ROOT);
#endif

#if !defined(MESH_ENABLED) || IS_ROOT
  // Fade in over ~1 second before handing off to the pot
#define FADEIN_MS 1000
#define FADEIN_STEPS 50
  for (int step = 0; step <= FADEIN_STEPS; step++) {
    int pot_raw = mux_read_ch(POT_CH);
    int target = BRI_MIN + ((4095 - pot_raw) * (BRI_MAX - BRI_MIN)) / 4095;
    int bri = (target * step) / FADEIN_STEPS;
    led_viz_set_brightness((uint8_t)bri);
    vTaskDelay(pdMS_TO_TICKS(FADEIN_MS / FADEIN_STEPS));
  }

  // Poll mux at ~100 Hz
  while (1) {
    mux_update();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
#else
  // Node board: no physical controls to poll. mesh_node's own task (spawned
  // above by mesh_init) applies the node's default/degraded state
  // immediately and drives every subsequent program/palette/brightness
  // change as root broadcasts arrive -- nothing left for app_main to do.
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif
}
