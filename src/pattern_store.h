#ifndef LASER_CAT_TOY_PATTERN_STORE_H
#define LASER_CAT_TOY_PATTERN_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pattern.h"

void pattern_store_init(void);
const char *pattern_store_get_status(void);
bool pattern_store_has_file(void);
bool pattern_store_begin_upload(uint32_t expected_len, char *message, size_t message_len);
bool pattern_store_write_upload_chunk(const uint8_t *data, uint32_t len, char *message, size_t message_len);
bool pattern_store_finish_upload(char *message, size_t message_len);
bool pattern_store_load_active_pack(const pattern_pack_t **pack, char *message, size_t message_len);

#endif
