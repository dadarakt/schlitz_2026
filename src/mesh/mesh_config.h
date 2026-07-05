#pragma once

// Ported from ../connected-leds/include/mesh_config.h, plus
// MESH_STATE_INTERVAL_MS for the program/palette/brightness broadcast.

#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF WIFI_IF_STA
#define ESPNOW_PMK "pmk1234567890123"
#define ESPNOW_LMK "lmk1234567890123"
#define ESPNOW_CHANNEL 1
#define ESPNOW_SEND_COUNT 100
#define ESPNOW_SEND_DELAY 1000
#define ESPNOW_SEND_LEN 32
#define ESPNOW_ENABLE_LONG_RANGE false
#define ESPNOW_ENABLE_POWER_SAVE false
#define ESPNOW_WAKE_WINDOW 50
#define ESPNOW_WAKE_INTERVAL 100
#define ESPNOW_QUEUE_SIZE 6
#define ESPNOW_MAXDELAY 512

#define ESPNOW_SYNC_INTERVAL_MS 5000 // Root broadcasts clock sync this often
#define ESPNOW_DISCOVERY_INTERVAL_MS                                          \
  2000                               // Root broadcasts discovery this often
#define ESPNOW_NODE_TIMEOUT_MS 10000 // Node resets if no sync for this long

// Root re-broadcasts program/palette/brightness this often even without a
// change, so a late-joining or reconnecting node converges on current state
// quickly rather than waiting for the next button press or pot movement.
#define MESH_STATE_INTERVAL_MS 1000

// How often root_task wakes up (when otherwise idle) to check whether
// there's anything to do -- a pending state change, a due sync, a due
// discovery broadcast. This is *not* how often those things actually
// happen (each has its own interval above, checked via tick difference);
// it's the worst-case latency between e.g. a button press setting the
// state-dirty flag and root_task actually noticing and sending it. Keep
// this well under MESH_STATE_INTERVAL_MS so a live change doesn't lag
// behind by whatever's left of a stale poll cycle.
#define MESH_ROOT_POLL_MS 50
