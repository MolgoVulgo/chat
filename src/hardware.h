#ifndef LASER_CAT_TOY_HARDWARE_H
#define LASER_CAT_TOY_HARDWARE_H

#include <stdbool.h>
#include <stdint.h>

void hardware_init(void);
void hardware_laser_init_safe_state(void);
void hardware_reset_position(void);
void hardware_set_position_from_coord(int16_t x, int16_t y);
void hardware_servo_set_enabled(bool enabled);
void hardware_laser_set(bool enabled);
void hardware_laser_set_raw_level(bool high);
bool hardware_laser_is_on(void);
void hardware_servo_pwm_task(void *arg);

#endif
