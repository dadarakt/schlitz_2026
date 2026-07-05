#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mesh_common.h"
#include <stdbool.h>
#include <stdint.h>

void mesh_node_start(send_param_t *send_param, QueueHandle_t queue);

// Implemented by application code (main.c, node build only). Called by
// mesh_node whenever the board's displayed program/palette/brightness
// should change:
//   - Once at startup, before anything has ever been heard from the root
//     (program_idx/palette_idx/brightness are the node's own boot
//     defaults, connected=false).
//   - Whenever a state broadcast arrives from the root (connected=true).
//   - Whenever the root is presumed lost (no sync for
//     ESPNOW_NODE_TIMEOUT_MS) -- re-applied with the same
//     program/palette/brightness as last received (the pattern/palette
//     itself doesn't change), but connected=false, so the app can dim as a
//     soft diagnostic cue without this module needing to know anything
//     about brightness scaling.
void mesh_node_apply_state(uint8_t program_idx, uint8_t palette_idx,
                           uint8_t brightness, bool connected);
