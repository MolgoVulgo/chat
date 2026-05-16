#include "main.h"

#include <stdlib.h>

#include "game.h"
#include "hardware.h"
#include "logging.h"
#include "ota.h"
#include "web.h"

#include "esp_common.h"
#include "freertos/task.h"
#include "uart.h"

uint32 user_rf_cal_sector_set(void)
{
    flash_size_map size_map = system_get_flash_size_map();

    switch (size_map) {
    case FLASH_SIZE_4M_MAP_256_256:
        return 128 - 5;
    case FLASH_SIZE_8M_MAP_512_512:
        return 256 - 5;
    case FLASH_SIZE_16M_MAP_512_512:
    case FLASH_SIZE_16M_MAP_1024_1024:
        return 512 - 5;
    case FLASH_SIZE_32M_MAP_512_512:
    case FLASH_SIZE_32M_MAP_1024_1024:
        return 1024 - 5;
    case FLASH_SIZE_64M_MAP_1024_1024:
        return 2048 - 5;
    case FLASH_SIZE_128M_MAP_1024_1024:
        return 4096 - 5;
    default:
        return 0;
    }
}

void user_init(void)
{
    hardware_laser_init_safe_state();
    UART_SetBaudrate(UART0, SERIAL_BAUD_RATE);
    srand((unsigned)(0x8266u ^ (uint32_t)xTaskGetTickCount()));

    hardware_init();
    game_set_enabled(false);
    web_portal_init();
    ota_init();

    APP_LOG("Laser Cat Toy ESP8266 RTOS start - pattern engine");
    APP_LOG("servo_h_gpio=%d servo_v_gpio=%d laser_gpio=%d",
            SERVO_HORIZONTAL_GPIO, SERVO_VERTICAL_GPIO, LASER_GPIO);
    APP_LOG("config_ap_ssid=%s path=/wifi", CONFIG_AP_SSID);
    APP_LOG("ota_running=%s ota_upload_expected=%s",
            ota_get_running_bin_name(), ota_get_expected_upload_bin_name());

    xTaskCreate(hardware_servo_pwm_task, "servo_pwm", 384, NULL, 5, NULL);
    xTaskCreate(game_movement_task, "movement", 768, NULL, 3, NULL);
    xTaskCreate(web_http_server_task, "http", 3072, NULL, 4, NULL);
    xTaskCreate(web_dns_server_task, "dns", 768, NULL, 4, NULL);
    xTaskCreate(web_wifi_status_task, "wifi_status", 512, NULL, 2, NULL);
}
