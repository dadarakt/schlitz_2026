// Ported from ../connected-leds/src/mesh_common.c -- generic ESP-NOW
// transport, unchanged (no LED-specific code lives here).

#include "mesh_common.h"
#include "esp_crc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "mesh_config.h"
#include "mesh_node.h"
#include "mesh_root.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mesh_common";
static uint16_t s_espnow_seq[DATA_MAX] = {0, 0};
static QueueHandle_t s_espnow_queue;

// ESP-NOW callbacks

static void send_cb(const esp_now_send_info_t *tx_info,
                    esp_now_send_status_t status) {
  espnow_event_t evt;
  event_send_cb_t *send_cb = &evt.info.send_cb;

  if (tx_info == NULL) {
    ESP_LOGE(TAG, "Send cb arg error");
    return;
  }

  evt.id = ESPNOW_SEND_CB;
  memcpy(send_cb->mac_addr, tx_info->des_addr, ESP_NOW_ETH_ALEN);
  send_cb->status = status;
  if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
    ESP_LOGW(TAG, "Send send queue fail");
  }
}

static void recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data,
                    int len) {
  espnow_event_t evt;
  event_recv_cb_t *recv_cb = &evt.info.recv_cb;
  uint8_t *mac_addr = recv_info->src_addr;
  uint8_t *des_addr = recv_info->des_addr;

  if (mac_addr == NULL || data == NULL || len <= 0) {
    ESP_LOGE(TAG, "Receive cb arg error");
    return;
  }

  if (IS_BROADCAST_ADDR(des_addr)) {
    ESP_LOGD(TAG, "Receive broadcast ESPNOW data");
  } else {
    ESP_LOGD(TAG, "Receive unicast ESPNOW data");
  }

  evt.id = ESPNOW_RECV_CB;
  memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  recv_cb->data = malloc(len);
  if (recv_cb->data == NULL) {
    ESP_LOGE(TAG, "Malloc receive data fail");
    return;
  }
  memcpy(recv_cb->data, data, len);
  recv_cb->data_len = len;
  if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
    ESP_LOGW(TAG, "Send receive queue fail");
    free(recv_cb->data);
  }
}

// Data parsing and preparation

int espnow_data_parse(uint8_t *data, uint16_t data_len, uint8_t *payload_type,
                      uint16_t *seq, uint32_t *magic) {
  espnow_data_t *buf = (espnow_data_t *)data;
  uint16_t crc, crc_cal = 0;

  if (data_len < sizeof(espnow_data_t)) {
    ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
    return -1;
  }

  *payload_type = buf->payload_type;
  *seq = buf->seq_num;
  *magic = buf->magic;
  crc = buf->crc;
  buf->crc = 0;
  crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

  if (crc_cal == crc) {
    return buf->type;
  }

  return -1;
}

void espnow_data_prepare(send_param_t *send_param) {
  espnow_data_t *buf = (espnow_data_t *)send_param->buffer;
  buf->type =
      IS_BROADCAST_ADDR(send_param->dest_mac) ? DATA_BROADCAST : DATA_UNICAST;
  buf->seq_num = s_espnow_seq[buf->type]++;
  buf->crc = 0;
  buf->magic = send_param->magic;
  buf->payload_type = PAYLOAD_TYPE_NONE;

  send_param->len = sizeof(espnow_data_t);

  buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

void espnow_data_prepare_payload(send_param_t *send_param,
                                 payload_type_t payload_type,
                                 const void *payload, size_t payload_len) {
  espnow_data_t *buf = (espnow_data_t *)send_param->buffer;
  buf->type =
      IS_BROADCAST_ADDR(send_param->dest_mac) ? DATA_BROADCAST : DATA_UNICAST;
  buf->seq_num = s_espnow_seq[buf->type]++;
  buf->magic = send_param->magic;
  buf->payload_type = payload_type;
  if (payload && payload_len > 0) {
    memcpy(buf->payload, payload, payload_len);
  }
  send_param->len = sizeof(espnow_data_t) + payload_len;
  buf->crc = 0;
  buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

// Peer management

bool espnow_add_peer(const uint8_t *mac_addr) {
  if (esp_now_is_peer_exist(mac_addr)) {
    return false;
  }

  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL) {
    ESP_LOGE(TAG, "Malloc peer information fail");
    return false;
  }
  memset(peer, 0, sizeof(esp_now_peer_info_t));
  peer->channel = ESPNOW_CHANNEL;
  peer->ifidx = ESPNOW_WIFI_IF;
  peer->encrypt = false;
  memcpy(peer->peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(peer));
  free(peer);

  return true;
}

void espnow_deinit(send_param_t *send_param, QueueHandle_t queue) {
  free(send_param->buffer);
  free(send_param);
  vQueueDelete(queue);
  esp_now_deinit();
}

// Initialization

static void wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if ESPNOW_ENABLE_LONG_RANGE
  ESP_ERROR_CHECK(esp_wifi_set_protocol(
      ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                          WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
#endif
}

static esp_err_t espnow_init(bool is_root) {
  send_param_t *send_param;

  s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
  if (s_espnow_queue == NULL) {
    ESP_LOGE(TAG, "Create queue fail");
    return ESP_FAIL;
  }

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
  ESP_ERROR_CHECK(esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW));
  ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(
      CONFIG_ESPNOW_WAKE_INTERVAL));
#endif
  ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)ESPNOW_PMK));

  // Add broadcast peer
  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL) {
    ESP_LOGE(TAG, "Malloc peer information fail");
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
  }
  memset(peer, 0, sizeof(esp_now_peer_info_t));
  peer->channel = ESPNOW_CHANNEL;
  peer->ifidx = ESPNOW_WIFI_IF;
  peer->encrypt = false;
  memcpy(peer->peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(peer));
  free(peer);

  // Initialize sending parameters
  send_param = malloc(sizeof(send_param_t));
  if (send_param == NULL) {
    ESP_LOGE(TAG, "Malloc send parameter fail");
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
  }
  memset(send_param, 0, sizeof(send_param_t));
  send_param->delay = ESPNOW_SEND_DELAY;
  send_param->len = ESPNOW_SEND_LEN;
  send_param->buffer = malloc(ESPNOW_SEND_LEN);
  if (send_param->buffer == NULL) {
    ESP_LOGE(TAG, "Malloc send buffer fail");
    free(send_param);
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
  }

  if (is_root) {
    ESP_LOGI(TAG, "Initializing as ROOT");
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->magic = UINT32_MAX;
    memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    espnow_data_prepare(send_param);
    mesh_root_start(send_param, s_espnow_queue);
  } else {
    ESP_LOGI(TAG, "Initializing as NODE");
    send_param->unicast = false;
    send_param->broadcast = false;
    send_param->magic = 0;
    memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    espnow_data_prepare(send_param);
    mesh_node_start(send_param, s_espnow_queue);
  }

  return ESP_OK;
}

void mesh_init(bool is_root) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init();
  espnow_init(is_root);
}
