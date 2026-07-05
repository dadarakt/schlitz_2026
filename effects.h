#pragma once
#include <led_viz.h>
#include <stdbool.h>

// Implemented in effects.c -- see there for the full strobe/flash
// mechanism. These are the mesh-facing surface: root reads its own current
// flash state to include in the periodic mesh state broadcast (see
// mesh_root_notify_state), and the node applies whatever it's told instead
// of picking its own color (see mesh_node.c's handle_state_msg) -- so both
// boards show the exact same flash, not independently-random colors.
bool flash_is_held(void);
RGB flash_get_color(void);
void flash_apply_remote(bool held, RGB color);
