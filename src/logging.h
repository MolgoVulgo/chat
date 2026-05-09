#ifndef LASER_CAT_TOY_LOGGING_H
#define LASER_CAT_TOY_LOGGING_H

#include "esp_common.h"

#ifndef APP_LOG_ENABLED
#define APP_LOG_ENABLED 0
#endif

#ifndef PATTERN_LOG_ENABLED
#define PATTERN_LOG_ENABLED 0
#endif

#ifndef WEB_LOG_ENABLED
#define WEB_LOG_ENABLED 0
#endif

#ifndef WEB_DEBUG_LOG_ENABLED
#define WEB_DEBUG_LOG_ENABLED 0
#endif

#if APP_LOG_ENABLED
#define APP_LOG(fmt, ...) os_printf("[app] " fmt "\n", ##__VA_ARGS__)
#else
#define APP_LOG(fmt, ...)
#endif

#if PATTERN_LOG_ENABLED
#define PATTERN_LOG(fmt, ...) os_printf("[pattern] " fmt "\n", ##__VA_ARGS__)
#else
#define PATTERN_LOG(fmt, ...)
#endif

#if WEB_LOG_ENABLED
#define WEB_LOG(fmt, ...) os_printf("[web] " fmt "\n", ##__VA_ARGS__)
#else
#define WEB_LOG(fmt, ...)
#endif

#if WEB_DEBUG_LOG_ENABLED
#define WEB_DEBUG_LOG(fmt, ...) os_printf("[web:debug] " fmt "\n", ##__VA_ARGS__)
#else
#define WEB_DEBUG_LOG(fmt, ...)
#endif

#endif
