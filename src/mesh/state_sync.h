#pragma once
#include <stdint.h>

// Broadcast periodically (and immediately on change) by the root so nodes
// mirror its active program/palette/brightness/flash. One-way -- nodes
// never report state back, only apply what they're told (see
// mesh_node.h's mesh_node_apply_state and effects.h's flash_apply_remote).
//
// flash_r/g/b carry root's *exact* already-computed flash color (after its
// own random palette pick + white-blend), not just a palette index -- so
// the node reproduces the identical color instead of independently
// sampling its own.
typedef struct {
  uint8_t program_idx;
  uint8_t palette_idx;
  uint8_t brightness;
  uint8_t flash_held; // 0 or 1
  uint8_t flash_r;
  uint8_t flash_g;
  uint8_t flash_b;
} __attribute__((packed)) state_msg_t;
