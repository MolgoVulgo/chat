/* Minimal builtin fallback pack: capture + soft_zigzag_insect. */
#include <stddef.h>

#include "esp_common.h"
#include "default_patterns.h"

#define BUILD_ASSERT(name, cond) typedef char build_assert_##name[(cond) ? 1 : -1]

static const pattern_step_t steps_soft_zigzag_insect[] ICACHE_RODATA_ATTR = {
    { STEP_HOLD, true, -50, -50, 2600, 0 },
    { STEP_MOVE, true, 30, -10, 3000, 0 },
    { STEP_MOVE, true, -20, 50, 2900, 0 },
    { STEP_MOVE, true, 80, 80, 3300, 0 },
    { STEP_HOLD, true, 80, 80, 2600, 0 },
    { STEP_MOVE, true, 10, 130, 3200, 0 },
    { STEP_MOVE, true, 120, 160, 3400, 0 },
    { STEP_JITTER, true, 120, 160, 2000, 14 },
    { STEP_MOVE, true, 200, 100, 3200, 0 },
    { STEP_MOVE, true, 140, 20, 3500, 0 },
    { STEP_HOLD, true, 140, 20, 3000, 0 },
    { STEP_MOVE, true, 240, -30, 3600, 0 },
    { STEP_MOVE, true, 160, -90, 3500, 0 },
    { STEP_MOVE, true, 50, -70, 3800, 0 },
    { STEP_HOLD, true, 50, -70, 3400, 0 },
    { STEP_MOVE, true, -50, -110, 4200, 0 },
    { STEP_MOVE, true, -130, -40, 3800, 0 },
    { STEP_JITTER, true, -130, -40, 2000, 12 },
    { STEP_MOVE, true, -40, 0, 3300, 0 },
    { STEP_MOVE, true, 20, -60, 3400, 0 },
    { STEP_HOLD, true, 20, -60, 4000, 0 },
    { STEP_MOVE, true, 100, -120, 3600, 0 },
    { STEP_HOLD, true, 100, -120, 4200, 0 },
};
BUILD_ASSERT(soft_zigzag_insect_step_count_fits, (sizeof(steps_soft_zigzag_insect) / sizeof(steps_soft_zigzag_insect[0])) <= 65535);

static const pattern_step_t steps_capture[] ICACHE_RODATA_ATTR = {
    { STEP_MOVE, true, 160, -180, 2800, 0 },
    { STEP_MOVE, true, 60, -280, 3000, 0 },
    { STEP_MOVE, true, 0, -350, 3200, 0 },
    { STEP_HOLD, true, 0, -350, 6000, 0 },
    { STEP_JITTER, true, 0, -350, 2600, 10 },
    { STEP_OFF_HOLD, false, 0, 0, 1800, 0 },
};
BUILD_ASSERT(capture_step_count_fits, (sizeof(steps_capture) / sizeof(steps_capture[0])) <= 65535);

BUILD_ASSERT(default_pattern_count_fits, 2 <= 65535);
BUILD_ASSERT(default_total_steps_fits, 29 <= 360);

static const pattern_t default_patterns[] ICACHE_RODATA_ATTR = {
    { "soft_zigzag_insect", "Petit insecte controle", 9, (uint16_t)(sizeof(steps_soft_zigzag_insect) / sizeof(steps_soft_zigzag_insect[0])), steps_soft_zigzag_insect },
    { "capture", "Capture", 0, (uint16_t)(sizeof(steps_capture) / sizeof(steps_capture[0])), steps_capture },
};

const pattern_pack_t default_pattern_pack ICACHE_RODATA_ATTR = {
    default_patterns,
    (uint16_t)(sizeof(default_patterns) / sizeof(default_patterns[0])),
    4,
    "compiled builtin(2)",
    NULL,
    0
};
