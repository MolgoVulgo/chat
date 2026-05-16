#ifndef LASER_CAT_TOY_APP_UTIL_H
#define LASER_CAT_TOY_APP_UTIL_H

#include <stdint.h>

#include "main.h"
#include "freertos/FreeRTOS.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static inline portTickType ms_to_ticks_min1(uint32_t ms)
{
    portTickType ticks = (portTickType)((ms + portTICK_RATE_MS - 1) / portTICK_RATE_MS);
    return ticks == 0 ? 1 : ticks;
}

static inline int16_t clamp_coord(int32_t v)
{
    if (v < COORD_MIN) {
        return COORD_MIN;
    }
    if (v > COORD_MAX) {
        return COORD_MAX;
    }
    return (int16_t)v;
}

#endif
