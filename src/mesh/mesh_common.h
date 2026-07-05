#pragma once

// Ported from ../connected-leds/include/mesh_common.h -- generic ESP-NOW
// transport (framing, CRC, peer management), unchanged. Adds
// PAYLOAD_TYPE_STATE for the program/palette/brightness broadcast that
// schlitzerei_fw needs on top of the original PTP-style clock sync.

#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                    0xFF, 0xFF, 0xFF};

//// Example code from
/// https://github.com/espressif/esp-idf/blob/v5.5.2/examples/wifi/espnow/main/espnow_example.h

#define IS_BROADCAST_ADDR(addr)                                                \
  (memcmp(addr, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

typedef enum {
  ESPNOW_SEND_CB,
  ESPNOW_RECV_CB,
} espnow_event_id_t;

typedef enum {
  PAYLOAD_TYPE_NONE,
  PAYLOAD_TYPE_SYNC,
  PAYLOAD_TYPE_DELAY_REQ,
  PAYLOAD_TYPE_DELAY_RESP,
  PAYLOAD_TYPE_STATE, // program_idx/palette_idx/brightness, root -> node
} payload_type_t;

typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  esp_now_send_status_t status;
} event_send_cb_t;

typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint8_t *data;
  int data_len;
} event_recv_cb_t;

typedef union {
  event_send_cb_t send_cb;
  event_recv_cb_t recv_cb;
} espnow_event_info_t;

/* When ESPNOW sending or receiving callback function is called, post event to
 * ESPNOW task. */
typedef struct {
  espnow_event_id_t id;
  espnow_event_info_t info;
} espnow_event_t;

enum {
  DATA_BROADCAST,
  DATA_UNICAST,
  DATA_MAX,
};

typedef struct {
  uint8_t type;         // Broadcast or unicast ESPNOW data.
  uint8_t payload_type; // payload_type_t value
  uint16_t seq_num;   // Sequence number of ESPNOW data.
  uint16_t crc;       // CRC16 value of ESPNOW data.
  uint32_t magic;     // Magic number used to determine leadership
  uint8_t payload[0]; // payload data bytes
} __attribute__((packed)) espnow_data_t;

/* Parameters of sending ESPNOW data. */
typedef struct {
  bool unicast;
  bool broadcast;
  uint32_t magic;  // Magic number used to determine leadership
  uint16_t delay;  // Delay between sending two ESPNOW data, unit: ms.
  int len;         // Length of ESPNOW data to be sent, unit: byte.
  uint8_t *buffer; // Buffer pointing to ESPNOW data.
  uint8_t dest_mac[ESP_NOW_ETH_ALEN]; // MAC address of destination device.
} send_param_t;

// Initialization
void mesh_init(bool is_root);

// Shared ESP-NOW functions
int espnow_data_parse(uint8_t *data, uint16_t data_len, uint8_t *payload_type,
                      uint16_t *seq, uint32_t *magic);
void espnow_data_prepare(send_param_t *send_param);
void espnow_data_prepare_payload(send_param_t *send_param,
                                 payload_type_t payload_type,
                                 const void *payload, size_t payload_len);
bool espnow_add_peer(const uint8_t *mac_addr);
void espnow_deinit(send_param_t *send_param, QueueHandle_t queue);
