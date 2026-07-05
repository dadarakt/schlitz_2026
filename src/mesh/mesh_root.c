// Adapted from ../connected-leds/src/mesh_root.c. Changes from the original
// prototype:
//   - sync_send() seeds t_1 from led_viz_get_elapsed_us() (this board's own
//     SDK animation clock) instead of the raw esp_timer_get_time(), so the
//     offset the node derives from the exchange can be fed straight into
//     led_viz_set_clock_offset_us() with no extra epoch bookkeeping.
//   - A program/palette/brightness "state" broadcast rides alongside the
//     existing clock sync: sent immediately when mesh_root_notify_state()
//     reports a change, and re-sent periodically regardless so a
//     late-joining or reconnecting node picks up current state quickly.
//   - Discovery broadcasts keep flowing periodically even after a peer has
//     joined (not just while has_peers is false, which only ever happens
//     once): a node that reboots comes back up waiting specifically for
//     one of these bare broadcasts to rejoin, and has_peers latching true
//     forever means that's the only way it gets back in. Any join/rejoin
//     ack also now unconditionally forces an immediate sync + state
//     resend, not just the very first one.
// The original's status-LED (update_led()) calls are dropped -- root's own
// bars already show the normal pattern regardless of peer connectivity, so
// there's nothing extra to indicate here.

#include "mesh_root.h"
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

static const char *TAG = "mesh_root";

typedef struct {
  send_param_t *send_param;
  QueueHandle_t queue;
} root_task_params_t;

// --- State broadcast (program/palette/brightness) ---

static volatile uint8_t s_program_idx = 0;
static volatile uint8_t s_palette_idx = 0;
static volatile uint8_t s_brightness = 0;
static volatile bool s_state_dirty = true; // broadcast once on startup

void mesh_root_notify_state(uint8_t program_idx, uint8_t palette_idx,
                            uint8_t brightness) {
  if (program_idx != s_program_idx || palette_idx != s_palette_idx ||
      brightness != s_brightness) {
    s_program_idx = program_idx;
    s_palette_idx = palette_idx;
    s_brightness = brightness;
    s_state_dirty = true;
  }
}

static void state_send(send_param_t *send_param) {
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  state_msg_t msg = {
      .program_idx = s_program_idx,
      .palette_idx = s_palette_idx,
      .brightness = s_brightness,
  };
  espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_STATE, &msg,
                              sizeof(msg));
  if (esp_now_send(send_param->dest_mac, send_param->buffer,
                   send_param->len) != ESP_OK) {
    ESP_LOGE(TAG, "State send error");
  }
}

// --- Time sync helpers ---

static void sync_send(send_param_t *send_param, uint32_t *sync_id) {
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  // Seed with this board's own elapsed SDK animation time (not the raw
  // esp_timer reading) -- see file header.
  sync_msg_t sync = {.t_1 = led_viz_get_elapsed_us(), .sync_id = (*sync_id)++};
  espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_SYNC, &sync,
                              sizeof(sync));
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Sync send error");
  }
  ESP_LOGI(TAG, "Sent sync id=%lu t1=%lld", (unsigned long)sync.sync_id,
           sync.t_1);
}

static void sync_handle_delay_req(send_param_t *send_param, uint8_t *data,
                                  const uint8_t *node_mac) {
  espnow_data_t *buf = (espnow_data_t *)data;
  delay_request_t *dreq = (delay_request_t *)buf->payload;
  uint32_t req_sync_id = dreq->sync_id;

  // Temporarily switch to unicast for this node
  memcpy(send_param->dest_mac, node_mac, ESP_NOW_ETH_ALEN);

  delay_response_t dresp = {.t_4 = esp_timer_get_time(),
                            .sync_id = req_sync_id};
  espnow_data_prepare_payload(send_param, PAYLOAD_TYPE_DELAY_RESP, &dresp,
                              sizeof(dresp));
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Delay response send error");
  }
  ESP_LOGI(TAG, "Sent delay_response id=%lu t4=%lld",
           (unsigned long)req_sync_id, dresp.t_4);

  // Restore broadcast MAC
  memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
}

// --- Connection handling ---

// Returns true if a new node was added.
static bool handle_node_response(const uint8_t *mac_addr) {
  return espnow_add_peer(mac_addr);
}

// --- Root task ---

static void root_task(void *pvParameter) {
  root_task_params_t *params = (root_task_params_t *)pvParameter;
  send_param_t *send_param = params->send_param;
  QueueHandle_t queue = params->queue;
  free(params);

  espnow_event_t evt;
  uint8_t recv_payload_type = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;

  uint32_t sync_id = 0;
  bool has_peers = false;
  TickType_t last_sync_tick = 0;
  TickType_t last_state_tick = 0;
  TickType_t last_discovery_tick = 0;

  vTaskDelay(pdMS_TO_TICKS(3000));
  ESP_LOGI(TAG, "Starting broadcast loop");

  // Send initial discovery broadcast
  espnow_data_prepare(send_param);
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Send error");
    espnow_deinit(send_param, queue);
    vTaskDelete(NULL);
  }

  // Polls frequently so a state change (button/pot on root) gets noticed
  // and sent quickly -- MESH_STATE_INTERVAL_MS/ESPNOW_SYNC_INTERVAL_MS/
  // ESPNOW_DISCOVERY_INTERVAL_MS below are still what actually paces each
  // kind of broadcast; this just controls how promptly we check them.
  TickType_t wait_ticks = pdMS_TO_TICKS(MESH_ROOT_POLL_MS);

  while (true) {
    BaseType_t got = xQueueReceive(queue, &evt, wait_ticks);

    if (got != pdTRUE) {
      // Timeout: check if it's time for a sync, a state resend, or discovery
      TickType_t now = xTaskGetTickCount();

      if (has_peers &&
          (now - last_sync_tick) >= pdMS_TO_TICKS(ESPNOW_SYNC_INTERVAL_MS)) {
        sync_send(send_param, &sync_id);
        last_sync_tick = now;
      }

      if (has_peers &&
          (s_state_dirty || (now - last_state_tick) >=
                                pdMS_TO_TICKS(MESH_STATE_INTERVAL_MS))) {
        state_send(send_param);
        s_state_dirty = false;
        last_state_tick = now;
      }

      // Keep sending discovery broadcasts periodically even once we have a
      // peer -- if that board reboots, it comes back up in
      // NODE_STATE_UNCONNECTED and can *only* rejoin via one of these bare
      // broadcasts (mesh_node.c's UNCONNECTED handler specifically waits
      // for PAYLOAD_TYPE_NONE, not SYNC/STATE). Without this, has_peers
      // latching true forever would mean a rebooted node has no way back
      // in. Harmless overhead either way: a handful of tiny broadcasts
      // every couple seconds on an otherwise-idle channel.
      if (!has_peers || (now - last_discovery_tick) >=
                            pdMS_TO_TICKS(ESPNOW_DISCOVERY_INTERVAL_MS)) {
        memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
        espnow_data_prepare(send_param);
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Discovery send error");
        }
        ESP_LOGD(TAG, "Sent discovery broadcast");
        last_discovery_tick = now;
      }
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

      if (recv_payload_type == PAYLOAD_TYPE_DELAY_REQ) {
        sync_handle_delay_req(send_param, recv_cb->data, recv_cb->mac_addr);
      } else if (recv_payload_type == PAYLOAD_TYPE_NONE) {
        // Bare header = node join broadcast or ack -- either the very
        // first join, or a rejoin after the node's own reboot (its MAC is
        // already a known peer, so handle_node_response is a no-op, but
        // it still needs treating like a fresh join: force an immediate
        // sync + state broadcast rather than waiting out a full interval,
        // so it converges as fast as possible instead of sitting dimmed
        // for up to ESPNOW_SYNC_INTERVAL_MS/MESH_STATE_INTERVAL_MS.
        ESP_LOGI(TAG, "Received ack/join from node " MACSTR,
                 MAC2STR(recv_cb->mac_addr));
        if (handle_node_response(recv_cb->mac_addr)) {
          ESP_LOGI(TAG, "New peer added: " MACSTR, MAC2STR(recv_cb->mac_addr));
        }
        has_peers = true;
        last_sync_tick =
            xTaskGetTickCount() - pdMS_TO_TICKS(ESPNOW_SYNC_INTERVAL_MS);
        last_state_tick =
            xTaskGetTickCount() - pdMS_TO_TICKS(MESH_STATE_INTERVAL_MS);
        s_state_dirty = true;
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

void mesh_root_start(send_param_t *send_param, QueueHandle_t queue) {
  ESP_LOGI(TAG, "Starting root");

  root_task_params_t *params = malloc(sizeof(root_task_params_t));
  params->send_param = send_param;
  params->queue = queue;

  // Pinned to core 0, away from the LED strip refresh tasks (core 1, see
  // led_viz_esp32.c and main.c's led_task). Classic ESP32's RMT has no
  // DMA -- it refills its small hardware buffer via interrupt many times
  // per transmission, and any same-core work (originally just ADC reads,
  // now also ESP-NOW send/recv processing) that delays a refill corrupts
  // whatever bits were mid-flight, splicing a wrong color into a few
  // LEDs. Plain xTaskCreate() has no core affinity and could otherwise
  // land this on core 1 at the scheduler's discretion.
  xTaskCreatePinnedToCore(root_task, "mesh_root", 4096, params, 4, NULL, 0);
}
