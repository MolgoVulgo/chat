#ifndef LASER_CAT_TOY_PATTERN_DAT_H
#define LASER_CAT_TOY_PATTERN_DAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pattern.h"

bool pattern_dat_load(const uint8_t *data,
                      uint32_t len,
                      const pattern_pack_t **pack,
                      char *message,
                      size_t message_len);

#endif
