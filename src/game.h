#ifndef LASER_CAT_TOY_GAME_H
#define LASER_CAT_TOY_GAME_H

#include <stdbool.h>

#include "pattern.h"

void game_set_enabled(bool enabled);
bool game_is_enabled(void);
void game_movement_task(void *arg);
void game_use_default_patterns(void);
bool game_use_pattern_pack(const pattern_pack_t *pack, const char *status);
const pattern_pack_t *game_get_pattern_pack(void);
const char *game_get_pattern_status(void);
bool game_set_selected_pattern(int16_t pattern_index);
int16_t game_get_selected_pattern(void);
const char *game_get_selected_pattern_id(void);
void game_set_speed_percent(uint16_t speed_percent);
uint16_t game_get_speed_percent(void);

#endif
