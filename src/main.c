#include "main.h"

#include <stdlib.h>

#include "game.h"
#include "hardware.h"
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
    UART_SetBaudrate(UART0, SERIAL_BAUD_RATE);
    srand((unsigned)(0x8266u ^ (uint32_t)xTaskGetTickCount()));

    hardware_init();
    web_portal_init();

    os_printf("Laser Cat Toy ESP8266 RTOS start - pattern engine\n");
    os_printf("servo_h_gpio=%d servo_v_gpio=%d laser_gpio=%d\n",
              SERVO_HORIZONTAL_GPIO, SERVO_VERTICAL_GPIO, LASER_GPIO);
    os_printf("config_ap_ssid=%s path=/wifi\n", CONFIG_AP_SSID);

    xTaskCreate(hardware_servo_pwm_task, "servo_pwm", 384, NULL, 5, NULL);
    xTaskCreate(game_movement_task, "movement", 768, NULL, 3, NULL);
    xTaskCreate(web_http_server_task, "http", 1536, NULL, 4, NULL);
    xTaskCreate(web_dns_server_task, "dns", 768, NULL, 4, NULL);
    xTaskCreate(web_wifi_status_task, "wifi_status", 512, NULL, 2, NULL);
}
