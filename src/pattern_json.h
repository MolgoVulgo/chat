#ifndef LASER_CAT_TOY_PATTERN_JSON_H
#define LASER_CAT_TOY_PATTERN_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pattern.h"

bool pattern_json_load(const char *json,
                       uint32_t json_len,
                       const pattern_pack_t **pack,
                       char *message,
                       size_t message_len);

#endif
