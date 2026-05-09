#ifndef LASER_CAT_TOY_GAME_H
#define LASER_CAT_TOY_GAME_H

#include <stdbool.h>

void game_set_enabled(bool enabled);
bool game_is_enabled(void);
void game_movement_task(void *arg);

#endif
