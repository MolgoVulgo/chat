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
#define SERVO_MAX_US            2500

// -----------------------------------------------------------------------------
// Moteur de patterns
// -----------------------------------------------------------------------------

#define MOTION_TICK_MS                  20
#define PATTERN_TRANSITION_MIN_MS       400
#define PATTERN_TRANSITION_MAX_MS       800
#define PATTERN_CAPTURE_EVERY           5
#define JITTER_POINT_INTERVAL_MS        140

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
#define WIFI_SCAN_MAX_RESULTS   24

// -----------------------------------------------------------------------------
// OTA
// -----------------------------------------------------------------------------

#define OTA_MAX_FIRMWARE_SIZE   (960 * 1024)

void user_init(void);

#endif
