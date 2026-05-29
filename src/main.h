// Laser Cat Toy - ESP8266 RTOS SDK configuration
// Version avec moteur de patterns coordonnés.
//
// Cible : Wemos D1 mini Pro / ESP8266
// Alimentation : USB 5 V sur la carte, rail 5 V repris depuis la broche 5V.
// Laser : module 5 V piloté par MOSFET 2N7000, jamais directement par GPIO.

#ifndef LASER_CAT_TOY_MAIN_H
#define LASER_CAT_TOY_MAIN_H

#include <stdbool.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// Pins Wemos D1 mini Pro / ESP8266
// -----------------------------------------------------------------------------

#define SERVO_HORIZONTAL_GPIO   5       // D1 / GPIO5
#define SERVO_VERTICAL_GPIO     4       // D2 / GPIO4
#define LASER_GPIO              14      // D5 / GPIO14 -> gate MOSFET via 100 ohms
#define LASER_ACTIVE_LOW        0       // 0 = GPIO high allume, 1 = GPIO low allume

// -----------------------------------------------------------------------------
// Limites mécaniques servos
// -----------------------------------------------------------------------------

#define SERVO_HORIZONTAL_MIN    50
#define SERVO_HORIZONTAL_MAX    130

#define SERVO_VERTICAL_MIN      26
#define SERVO_VERTICAL_MAX      60

// Inversion éventuelle selon montage physique.
// 0 = normal, 1 = inversé.
#define SERVO_HORIZONTAL_INVERT 0
#define SERVO_VERTICAL_INVERT   0

// -----------------------------------------------------------------------------
// PWM servo
// -----------------------------------------------------------------------------

#define SERVO_PERIOD_MS         20
#define SERVO_MIN_US            500
#define SERVO_MAX_US            2400
#define SERVO_PULSE_ANGLE_MIN   20
#define SERVO_PULSE_ANGLE_MAX   160

// -----------------------------------------------------------------------------
// Moteur de patterns
// -----------------------------------------------------------------------------

#define MOTION_TICK_MS                  20
#define PATTERN_TRANSITION_MIN_MS       400
#define PATTERN_TRANSITION_MAX_MS       800
#define PATTERN_CAPTURE_EVERY           5
#define JITTER_POINT_INTERVAL_MS        140

#define PATTERN_SCHEMA                  "laser_cat_patterns.v1"
#define PATTERN_JSON_UPLOAD_ENABLED     0
#define PATTERN_JSON_UPLOAD_MAX_BYTES   (64 * 1024)
#define PATTERN_MAX_PATTERNS            16
#define PATTERN_MAX_TOTAL_STEPS         360
#define PATTERN_MAX_ID_LEN              32
#define PATTERN_MAX_NAME_LEN            48
#define PATTERN_MIN_DURATION_MS         100
#define PATTERN_MIN_VISIBLE_MS          1000
#define PATTERN_MAX_JITTER_AMPLITUDE    250
#define PATTERN_SPEED_MIN_PERCENT       25
#define PATTERN_SPEED_MAX_PERCENT       300
#define PATTERN_SPEED_DEFAULT_PERCENT   100

#ifndef DEBUG_HARDWARE_ENABLED
#define DEBUG_HARDWARE_ENABLED          0
#endif

#define GAME_SESSION_MAX_MS             (10 * 60 * 1000)
#define GAME_COOLDOWN_MS                (5 * 60 * 1000)
#define SERVO_REST_SETTLE_MS            500
#define LASER_PULSE_MIN_MS              100
#define LASER_PULSE_DEFAULT_MS          1000
#define LASER_PULSE_MAX_MS              3000
#define LASER_TEST_MAX_MS               (60 * 1000)

// Coordonnées normalisées internes : -1000 à +1000.
// x = -1000 gauche, +1000 droite.
// y = -1000 bas,    +1000 haut.
#define COORD_MIN               (-1000)
#define COORD_MAX               1000

// Position de repos au démarrage.
#define START_X                 0
#define START_Y                 0

// -----------------------------------------------------------------------------
// Serial
// -----------------------------------------------------------------------------

#define SERIAL_BAUD_RATE        115200

// -----------------------------------------------------------------------------
// WiFi / Web UI
// -----------------------------------------------------------------------------

#define CONFIG_AP_SSID          "LaserCatToy"
#define CONFIG_AP_PASSWORD      "lasercat123"
#define HTTP_PORT               80
#define DNS_PORT                53
#define CONFIG_AP_IP_A          192
#define CONFIG_AP_IP_B          168
#define CONFIG_AP_IP_C          4
#define CONFIG_AP_IP_D          1
#define WIFI_SCAN_MAX_RESULTS   5
#define HTTP_SOCKET_TIMEOUT_MS  3000

void user_init(void);

#endif
