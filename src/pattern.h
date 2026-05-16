#ifndef LASER_CAT_TOY_PATTERN_H
#define LASER_CAT_TOY_PATTERN_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STEP_HOLD,
    STEP_MOVE,
    STEP_JITTER,
    STEP_OFF_HOLD,
    STEP_OFF_MOVE
} pattern_step_type_t;

typedef struct {
    pattern_step_type_t type;
    bool laser;
    int16_t x;
    int16_t y;
    uint16_t duration_ms;
    uint16_t amplitude;
} pattern_step_t;

typedef struct {
    const char *id;
    const char *name;
    uint8_t weight;
    uint16_t step_count;
    const pattern_step_t *steps;
} pattern_t;

typedef struct {
    const pattern_t *patterns;
    uint16_t pattern_count;
    uint8_t capture_every;
    const char *source_name;
    const char *json_source;
    uint32_t json_source_len;
} pattern_pack_t;

#endif
