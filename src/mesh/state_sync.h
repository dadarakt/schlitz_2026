#pragma once
#include <stdint.h>

// Broadcast periodically (and immediately on change) by the root so nodes
// mirror its active program/palette/brightness. One-way -- nodes never
// report state back, only apply what they're told (see mesh_node.h's
// mesh_node_apply_state).
typedef struct {
  uint8_t program_idx;
  uint8_t palette_idx;
  uint8_t brightness;
} __attribute__((packed)) state_msg_t;
