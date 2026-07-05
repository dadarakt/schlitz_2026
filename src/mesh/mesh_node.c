// Adapted from ../connected-leds/src/mesh_node.c. Changes from the original
// prototype:
//   - sync_handle_delay_resp() feeds the computed offset straight into
//     led_viz_set_clock_offset_us() instead of connected-leds' own
//     update_led() status indicator (see mesh_root.c's header comment for
//     why no epoch bookkeeping is needed beyond that).
//   - Handles the new PAYLOAD_TYPE_STATE broadcast (program/palette/
//     brightness), applying it via mesh_node_apply_state() (implemented in
//     main.c, since only application code knows about palettes[]/NUM_*).
//   - Default/degraded state handling: this node always has *some*
//     program/palette/brightness applied, even before ever hearing from
//     root (fresh boot, root not yet in range) or after losing contact
//     with it (root out of range, restarted, or crashed) -- see
//     apply_current() below and the NODE_DEFAULT_* constants.

#include "mesh_node.h"
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
}

// --- Disconnected indicator ---

#define OFFLINE_GLIMMER_LEDS 30
#define OFFLINE_GLIMMER_PEAK 50 // faint -- comes from the color value itself,
#define OFFLINE_GLIMMER_FLOOR 10 // not from a separate brightness scale

void mesh_node_offline_overlay(double time_ms, PixelFunc pixel,
                               GetPixelFunc get_pixel, const Palette16 palette) {
  (void)get_pixel;
  (void)palette;
  if (s_connected) return;

  // Full global brightness while showing this -- the glimmer's own
  // faintness (OFFLINE_GLIMMER_PEAK/FLOOR) shouldn't be further scaled
  // down by whatever brightness root last sent before going away.
  led_viz_set_brightness(255);

  int n_strips = get_num_strips();
  for (int s = 0; s < n_strips; s++) {
    int n = get_strip_num_leds(s);
    RGB black = {0, 0, 0};
    for (int i = 0; i < n; i++) pixel(s, i, &black.r, &black.g, &black.b);

    int start = (n - OFFLINE_GLIMMER_LEDS) / 2;
    if (start < 0) start = 0;
    int end = start + OFFLINE_GLIMMER_LEDS;
    if (end > n) end = n;

    for (int i = start; i < end; i++) {
      // Slow per-pixel flicker (not a uniform pulse) via 1D noise walked
      // along the strip and across time -- reads as an organic "glimmer"
      // rather than a steady glow or independent per-pixel sparkle.
      uint8_t n8 = inoise8((uint16_t)(i * 9), 0, (uint16_t)(time_ms * 0.03));
      uint8_t r = OFFLINE_GLIMMER_FLOOR +
                  scale8(n8, OFFLINE_GLIMMER_PEAK - OFFLINE_GLIMMER_FLOOR);
      uint8_t g = 0, b = 0;
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
  int64_t t5 = esp_timer_get_time();
  espnow_data_t *buf = (espnow_data_t *)data;
  delay_response_t *dresp = (delay_response_t *)buf->payload;

  if ((uint32_t)dresp->sync_id != ss->pending_sync_id) {
    ESP_LOGW(TAG, "sync_id mismatch: got %ld expected %lu",
             (long)dresp->sync_id, (unsigned long)ss->pending_sync_id);
    return false;
  }

  int64_t rtt = t5 - ss->pending_t3;
  int64_t one_way = rtt / 2;
  // pending_t1 is root's own elapsed-SDK-clock reading (see mesh_root.c),
  // so this offset can be applied directly -- no separate epoch alignment
  // needed.
  int64_t clock_offset_us = (ss->pending_t1 + one_way) - ss->pending_t2;

  ESP_LOGI(TAG, "Sync id=%lu rtt=%lld us one_way=%lld us offset=%lld us",
           (unsigned long)ss->pending_sync_id, rtt, one_way, clock_offset_us);

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
