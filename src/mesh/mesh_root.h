#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mesh_common.h"
#include <stdbool.h>
#include <stdint.h>

void mesh_root_start(send_param_t *send_param, QueueHandle_t queue);

// Called by app code (the mux polling loop) whenever the active
// program/palette/brightness/flash may have changed. Cheap to call every
// tick -- only actually broadcasts when something changed or the periodic
// resend (MESH_STATE_INTERVAL_MS) is due. flash_r/g/b are only meaningful
// (and only need to be current) while flash_held is true.
void mesh_root_notify_state(uint8_t program_idx, uint8_t palette_idx,
                            uint8_t brightness, bool flash_held,
                            uint8_t flash_r, uint8_t flash_g, uint8_t flash_b);
