#pragma once
// Ported from ../connected-leds/include/ptp_sync.h, unchanged. t_4 is
// carried but not currently used in the offset calculation (mirrors the
// original prototype's simplified 2-timestamp round-trip approximation).
#include <stdint.h>

typedef struct {
  int64_t t_1;
  uint32_t sync_id;
} __attribute__((packed)) sync_msg_t;

typedef struct {
  uint32_t sync_id;
} __attribute__((packed)) delay_request_t;

typedef struct {
  int64_t t_4;
  int32_t sync_id;
} __attribute__((packed)) delay_response_t;
