#pragma once

// The build environment must define either ROLE_ROOT or ROLE_NODE, and
// MESH_ENABLED, whenever wireless sync is wanted at all -- see the
// schlitzerei_root / schlitzerei_node envs in platformio.ini. The plain
// schlitzerei env defines neither, and main.c runs standalone exactly as it
// always has.
// For example, in platformio.ini:
// build_flags = -DMESH_ENABLED -DROLE_ROOT  (for the root board)
// build_flags = -DMESH_ENABLED -DROLE_NODE  (for the node board)

#if defined(ROLE_ROOT)
#define IS_ROOT 1
#elif defined(ROLE_NODE)
#define IS_ROOT 0
#endif
