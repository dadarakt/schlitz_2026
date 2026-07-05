// Adapted from ../connected-leds/src/mesh_node.c. Changes from the original
// prototype:
//   - sync_handle_delay_resp() feeds the computed offset straight into
//     led_viz_set_clock_offset_us() instead of connected-leds' own
//     update_led() status indicator.
//   - sync_handle_delay_resp() now computes that offset via the full
//     4-timestamp PTP formula (using t_4, previously received but
//     discarded) instead of a 2-timestamp round-trip approximation -- see
//     mesh_root.c's header comment for why the original approximation had
//     a steady bias rather than random jitter.
//   - Handles the new PAYLOAD_TYPE_STATE broadcast (program/palette/
//     brightness), applying it via mesh_node_apply_state() (implemented in
//     main.c, since only application code knows about palettes[]/NUM_*).
//     Flash state rides along in the same message but is applied directly
//     via effects.h's flash_apply_remote(), bypassing mesh_node_apply_state
//     entirely -- flash is momentary, not part of the "current program"
//     concept that gets cached and re-applied on disconnect.
//   - Default/degraded state handling: this node always has *some*
//     program/palette/brightness applied, even before ever hearing from
//     root (fresh boot, root not yet in range) or after losing contact
//     with it (root out of range, restarted, or crashed) -- see
//     apply_current() below and the NODE_DEFAULT_* constants.

#include "mesh_node.h"
#include "../../effects.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_viz_esp32.h"
#include "mesh_common.h"
#include "mesh_config.h"
#include "ptp_sync.h"
#include "state_sync.h"
#include <math.h>
#include <string.h>

static const char *TAG = "mesh_node";

// Applied at boot, before this node has ever heard from the root: index 0
// on both program and palette (root and node build from the same
// programs[]/palettes[] arrays, so index 0 means the same thing on both),
// and a moderate fixed brightness in place of the root's own pot reading
// (this board has no pot of its own -- brightness always comes from root).
#define NODE_DEFAULT_PROGRAM_IDX 0
#define NODE_DEFAULT_PALETTE_IDX 0
#define NODE_DEFAULT_BRIGHTNESS 40

typedef enum {
  NODE_STATE_UNCONNECTED,
  NODE_STATE_CONNECTED,
} node_conn_state_t;

typedef struct {
  send_param_t *send_param;
  QueueHandle_t queue;
} node_task_params_t;

// --- Last-known state, re-applied (with connected=false) on disconnect ---

static uint8_t s_program_idx = NODE_DEFAULT_PROGRAM_IDX;
static uint8_t s_palette_idx = NODE_DEFAULT_PALETTE_IDX;
static uint8_t s_brightness = NODE_DEFAULT_BRIGHTNESS;

// Read by mesh_node_offline_overlay (runs in the LED task) -- written by
// node_task on every connect/disconnect transition. Plain volatile bool,
// matching the read/write-from-different-tasks pattern effects.c already
// uses for strobe_held/flash_held.
static volatile bool s_connected = false;

static void apply_current(bool connected) {
  s_connected = connected;
  mesh_node_apply_state(s_program_idx, s_palette_idx, s_brightness, connected);
}

static void handle_state_msg(uint8_t *data) {
  espnow_data_t *buf = (espnow_data_t *)data;
  state_msg_t *msg = (state_msg_t *)buf->payload;

  s_program_idx = msg->program_idx;
  s_palette_idx = msg->palette_idx;
  s_brightness = msg->brightness;
  apply_current(true);

  RGB flash_color = {msg->flash_r, msg->flash_g, msg->flash_b};
  flash_apply_remote(msg->flash_held != 0, flash_color);
}

// --- Disconnected indicator ---

// Faint amber/orange -- G is ~40% of R, giving an orange hue rather than
// pure red.
#define OFFLINE_GLIMMER_PEAK 28 // faint -- comes from the color value itself,
#define OFFLINE_GLIMMER_FLOOR 5 // not from a separate brightness scale
#define OFFLINE_GLIMMER_HALF_WIDTH 15.0 // ~30 LEDs wide at the base

// One full up+down traversal of the strip.
#define OFFLINE_MOVE_PERIOD_MS 4000.0
#define OFFLINE_PI 3.14159265358979323846

void mesh_node_offline_overlay(double time_ms, PixelFunc pixel,
                               GetPixelFunc get_pixel, const Palette16 palette) {
  (void)get_pixel;
  (void)palette;
  if (s_connected) return;

  // Full global brightness while showing this -- the glimmer's own
  // faintness (OFFLINE_GLIMMER_PEAK/FLOOR) shouldn't be further scaled
  // down by whatever brightness root last sent before going away.
  led_viz_set_brightness(255);

  // Smooth pendulum motion (sinusoidal ease, not linear) between the two
  // ends of the strip -- pure function of time, so multiple strips (and,
  // incidentally, multiple boards) stay in step with no extra state.
  double phase = fmod(time_ms, OFFLINE_MOVE_PERIOD_MS) / OFFLINE_MOVE_PERIOD_MS; // 0..1
  double eased = (sin(phase * 2.0 * OFFLINE_PI - OFFLINE_PI / 2.0) + 1.0) * 0.5; // 0..1

  int n_strips = get_num_strips();
  for (int s = 0; s < n_strips; s++) {
    int n = get_strip_num_leds(s);
    RGB black = {0, 0, 0};
    for (int i = 0; i < n; i++) pixel(s, i, &black.r, &black.g, &black.b);

    double center = eased * (double)(n - 1);

    for (int i = 0; i < n; i++) {
      double dist = fabs((double)i - center);
      if (dist >= OFFLINE_GLIMMER_HALF_WIDTH) continue; // stays black

      // Smooth (cosine) falloff from the moving center, not a hard-edged
      // window -- 1.0 right at center, tapering to 0 at the band's edges.
      double falloff = 0.5 * (1.0 + cos(OFFLINE_PI * dist / OFFLINE_GLIMMER_HALF_WIDTH));

      // Slow per-pixel flicker on top of the moving envelope, via 1D noise
      // walked along the strip and across time -- reads as an organic
      // "glimmer" rather than a steady glow or independent per-pixel
      // sparkle.
      uint8_t n8 = inoise8((uint16_t)(i * 9), 0, (uint16_t)(time_ms * 0.03));
      uint8_t flicker = OFFLINE_GLIMMER_FLOOR +
                        scale8(n8, OFFLINE_GLIMMER_PEAK - OFFLINE_GLIMMER_FLOOR);
      uint8_t level = (uint8_t)(flicker * falloff + 0.5);

      uint8_t r = level;
      uint8_t g = (uint8_t)(level * 2 / 5); // orange hue
      uint8_t b = 0;
      pixel(s, i, &r, &g, &b);
    }
  }
}

// --- Time sync state and helpers ---

typedef struct {
  int64_t pending_t1;
  int64_t pending_t2;
  int64_t pending_t3;
  uint32_t pending_sync_id;
  bool synced;
} sync_state_t;

static void sync_handle_sync_msg(sync_state_t *ss, send_param_t *send_param,
                                 uint8_t *data) {
  int64_t t2 = esp_timer_get_time();
  espnow_data_t *buf = (espnow_data_t *)data;
  sync_msg_t *sync = (sync_msg_t *)buf->payload;

  ss->pending_t1 = sync->t_1;
  ss->pending_t2 = t2;
  ss->pending_sync_id = sync->sync_id;

  ESP_LOGI(TAG, "Received sync id=%lu t1=%lld t2=%lld",
           (unsigned long)ss->pending_sync_id, ss->pending_t1, ss->pending_t2);

  delay_request_t dreq = {.sync_id = ss->pending_sync_id};
  ss->pending_t3 = esp_timer_get_time();
  espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_DELAY_REQ, &dreq,
                              sizeof(dreq));
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Delay request send error");
  }
}

// Returns true if the response was valid and sync was applied.
static bool sync_handle_delay_resp(sync_state_t *ss, uint8_t *data) {
  espnow_data_t *buf = (espnow_data_t *)data;
  delay_response_t *dresp = (delay_response_t *)buf->payload;

  if ((uint32_t)dresp->sync_id != ss->pending_sync_id) {
    ESP_LOGW(TAG, "sync_id mismatch: got %ld expected %lu",
             (long)dresp->sync_id, (unsigned long)ss->pending_sync_id);
    return false;
  }

  // Full 4-timestamp PTP offset: t_1/t_4 are root's elapsed-SDK-clock
  // readings (send SYNC / receive DELAY_REQ -- see mesh_root.c), t_2/t_3
  // are this board's own raw clock readings (receive SYNC / send
  // DELAY_REQ). This cancels a *symmetric* one-way delay by construction,
  // unlike the previous "half of a separately-measured round trip"
  // approximation, which had no way to account for any asymmetry between
  // the root->node and node->root legs (e.g. ESP-NOW's receive callback
  // running inside the driver's own dispatch task before reaching our
  // queue) and so carried a steady bias rather than random jitter.
  int64_t clock_offset_us =
      ((ss->pending_t1 + dresp->t_4) - (ss->pending_t2 + ss->pending_t3)) / 2;

  ESP_LOGI(TAG, "Sync id=%lu t1=%lld t2=%lld t3=%lld t4=%lld offset=%lld us",
           (unsigned long)ss->pending_sync_id, ss->pending_t1, ss->pending_t2,
           ss->pending_t3, dresp->t_4, clock_offset_us);

  led_viz_set_clock_offset_us(clock_offset_us);
  ss->synced = true;

  return true;
}

// --- Connection handling ---

// Returns true if the node successfully connected to the root.
static bool handle_root_broadcast(send_param_t *send_param,
                                  const uint8_t *mac_addr) {
  if (!esp_now_is_peer_exist(mac_addr)) {
    if (!espnow_add_peer(mac_addr)) {
      return false;
    }
  }

  memcpy(send_param->dest_mac, mac_addr, ESP_NOW_ETH_ALEN);
  espnow_data_prepare(send_param);
  esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len);
  ESP_LOGI(TAG, "Sent ack to root " MACSTR, MAC2STR(mac_addr));
  ESP_LOGI(TAG, "State=CONNECTED");
  return true;
}

// --- Node task ---

static void node_task(void *pvParameter) {
  node_task_params_t *params = (node_task_params_t *)pvParameter;
  send_param_t *send_param = params->send_param;
  QueueHandle_t queue = params->queue;
  free(params);

  espnow_event_t evt;
  uint8_t recv_payload_type = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;

  node_conn_state_t state = NODE_STATE_UNCONNECTED;
  sync_state_t ss = {0};
  TickType_t last_sync_tick = 0;

  ESP_LOGI(TAG, "State=UNCONNECTED, waiting for root broadcasts");

  // Never heard from root yet -- show the default program/palette, dimmed
  // as the "not yet synced" cue, rather than sitting dark or on stale
  // pixels from whatever ran before this.
  apply_current(false);

  while (true) {
    TickType_t wait_ticks;
    if (state == NODE_STATE_CONNECTED) {
      TickType_t now = xTaskGetTickCount();
      TickType_t elapsed = now - last_sync_tick;
      TickType_t timeout = pdMS_TO_TICKS(ESPNOW_NODE_TIMEOUT_MS);
      if (elapsed >= timeout) {
        wait_ticks = 0;
      } else {
        wait_ticks = timeout - elapsed;
      }
    } else {
      wait_ticks = portMAX_DELAY;
    }

    BaseType_t got = xQueueReceive(queue, &evt, wait_ticks);

    if (got != pdTRUE) {
      // Timeout: lost connection to root. Keep showing the last known
      // program/palette (don't yank the display to something else), just
      // dim it as a soft diagnostic cue -- and leave the clock offset
      // alone so the animation doesn't jump, it just free-runs from
      // wherever it was until resynced.
      ESP_LOGW(TAG, "Sync timeout, resetting to UNCONNECTED");
      state = NODE_STATE_UNCONNECTED;
      memset(&ss, 0, sizeof(ss));
      apply_current(false);
      // Don't leave a flash stuck on if root disappears mid-press -- the
      // offline overlay fully overrides the display anyway, but this keeps
      // state consistent for the moment before it kicks in.
      flash_apply_remote(false, (RGB){0, 0, 0});
      continue;
    }

    switch (evt.id) {
    case ESPNOW_SEND_CB: {
      event_send_cb_t *send_cb = &evt.info.send_cb;
      ESP_LOGD(TAG, "Sent to " MACSTR ", status: %d",
               MAC2STR(send_cb->mac_addr), send_cb->status);
      break;
    }
    case ESPNOW_RECV_CB: {
      event_recv_cb_t *recv_cb = &evt.info.recv_cb;

      int ret = espnow_data_parse(recv_cb->data, recv_cb->data_len,
                                  &recv_payload_type, &recv_seq, &recv_magic);
      if (ret < 0) {
        free(recv_cb->data);
        break;
      }

      if (state == NODE_STATE_UNCONNECTED && ret == DATA_BROADCAST &&
          recv_payload_type == PAYLOAD_TYPE_NONE) {
        ESP_LOGI(TAG, "Received broadcast from root " MACSTR,
                 MAC2STR(recv_cb->mac_addr));
        if (handle_root_broadcast(send_param, recv_cb->mac_addr)) {
          state = NODE_STATE_CONNECTED;
          last_sync_tick = xTaskGetTickCount();
        }
      } else if (state == NODE_STATE_CONNECTED &&
                 recv_payload_type == PAYLOAD_TYPE_SYNC) {
        last_sync_tick = xTaskGetTickCount();
        sync_handle_sync_msg(&ss, send_param, recv_cb->data);
      } else if (state == NODE_STATE_CONNECTED &&
                 recv_payload_type == PAYLOAD_TYPE_DELAY_RESP) {
        sync_handle_delay_resp(&ss, recv_cb->data);
      } else if (state == NODE_STATE_CONNECTED &&
                 recv_payload_type == PAYLOAD_TYPE_STATE) {
        last_sync_tick = xTaskGetTickCount();
        handle_state_msg(recv_cb->data);
      }

      free(recv_cb->data);
      break;
    }
    default:
      ESP_LOGE(TAG, "Callback type error: %d", evt.id);
      break;
    }
  }
}

void mesh_node_start(send_param_t *send_param, QueueHandle_t queue) {
  ESP_LOGI(TAG, "Starting node");

  node_task_params_t *params = malloc(sizeof(node_task_params_t));
  params->send_param = send_param;
  params->queue = queue;

  // Pinned to core 0 -- see mesh_root_start's comment (mesh_root.c) for
  // why: same reasoning applies here, this board also runs the LED strip
  // refresh tasks on core 1.
  xTaskCreatePinnedToCore(node_task, "mesh_node", 4096, params, 4, NULL, 0);
}
