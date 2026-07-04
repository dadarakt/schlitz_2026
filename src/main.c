#include "led_viz_esp32.h"
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>

// ets_delay_us is always available as an ESP32 ROM function
extern void ets_delay_us(unsigned int us);

#include "../programs.c"

static const char *TAG = "schlitzerei";

// ============================================================================
// Palette list
// ============================================================================

static const Palette16 *palettes[] = {
    &PALETTE_RAINBOW, &PALETTE_LAVA, &PALETTE_OCEAN, &PALETTE_FOREST,
    &PALETTE_PARTY,   &PALETTE_HEAT, &PALETTE_CLOUD,
};
#define NUM_PALETTES ((int)(sizeof(palettes) / sizeof(palettes[0])))

static int current_program_idx = 0;
static int current_palette_idx = 0;

// ============================================================================
// Mux configuration  (mirrors MuxCfg in MuxInput.h)
// ============================================================================

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

// Brightness range (0–255).  Pot at minimum → BRI_MIN, pot at maximum → BRI_MAX.
#define BRI_MIN 0
#define BRI_MAX 100

// ============================================================================
// Mux init / read
// ============================================================================

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

// ============================================================================
// Button + pot handling  (mirrors MuxInput.cpp logic)
// ============================================================================

static void mux_update(void) {
  static bool last_btn[4] = {false, false, false, false};
  static bool combo_active = false;

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
      // combo: both released → no-op for now (autocycle toggle not yet ported)
      combo_active = false;
    } else if (!combo_active) {
      current_program_idx = (current_program_idx + 1) % NUM_PROGRAMS;
      led_viz_set_program(current_program_idx);
      ESP_LOGI(TAG, "Program → %d", current_program_idx);
    }
  }

  // Button 1 released
  if (!btn[1] && last_btn[1]) {
    if (!combo_active) {
      current_palette_idx = (current_palette_idx + 1) % NUM_PALETTES;
      led_viz_set_palette(palettes[current_palette_idx]);
      ESP_LOGI(TAG, "Palette → %d", current_palette_idx);
    }
  }

  for (int i = 0; i < 4; i++)
    last_btn[i] = btn[i];

  // Button 2 (crowd blinder) and Button 3 (strobe) — not yet ported
}

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

  mux_init();

  // DEBUG: single strip for signal integrity testing
  //   0  bar_1  → GPIO 22  (WS2812)
  LedVizConfig config = {
      .gpio_pins = {22},
      .target_fps = 60,
  };

  if (led_viz_init(&config) != 0) {
    ESP_LOGE(TAG, "LED init failed");
    return;
  }

  led_viz_set_program(0);
  led_viz_set_palette(palettes[0]);
  led_viz_set_brightness(0); // start dark for fade-in

  // Run LED loop in its own task so app_main can poll the mux
  xTaskCreate(led_task, "led", 8192, NULL, 5, NULL);

  // Fade in over ~1 second before handing off to the pot
#define FADEIN_MS     1000
#define FADEIN_STEPS  50
  for (int step = 0; step <= FADEIN_STEPS; step++) {
    int pot_raw = mux_read_ch(POT_CH);
    int target  = BRI_MIN + ((4095 - pot_raw) * (BRI_MAX - BRI_MIN)) / 4095;
    int bri     = (target * step) / FADEIN_STEPS;
    led_viz_set_brightness((uint8_t)bri);
    vTaskDelay(pdMS_TO_TICKS(FADEIN_MS / FADEIN_STEPS));
  }

  // Poll mux at ~100 Hz
  while (1) {
    mux_update();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
