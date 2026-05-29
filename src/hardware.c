#include "hardware.h"

#include "app_util.h"
#include "logging.h"
#include "main.h"

#include "esp_common.h"
#include "freertos/task.h"
#include "gpio.h"

static volatile int servo_h_pos = 90;
static volatile int servo_v_pos = 90;
static volatile bool servo_enabled = false;
static volatile bool laser_on = false;

static uint8_t laser_gpio_level(bool enabled)
{
    return enabled ? (LASER_ACTIVE_LOW ? 0 : 1) : (LASER_ACTIVE_LOW ? 1 : 0);
}

static int coord_to_angle(int16_t coord, int min_angle, int max_angle, int invert)
{
    coord = clamp_coord(coord);

    if (invert) {
        coord = (int16_t)-coord;
    }

    int32_t span = (int32_t)max_angle - (int32_t)min_angle;
    int32_t v = (int32_t)(coord + 1000);
    int32_t angle = (int32_t)min_angle + ((v * span) / 2000);

    if (angle < min_angle) {
        angle = min_angle;
    }
    if (angle > max_angle) {
        angle = max_angle;
    }

    return (int)angle;
}

void hardware_set_position_from_coord(int16_t x, int16_t y)
{
    int16_t current_x = clamp_coord(x);
    int16_t current_y = clamp_coord(y);

    servo_h_pos = coord_to_angle(current_x,
                                 SERVO_HORIZONTAL_MIN,
                                 SERVO_HORIZONTAL_MAX,
                                 SERVO_HORIZONTAL_INVERT);

    servo_v_pos = coord_to_angle(current_y,
                                 SERVO_VERTICAL_MIN,
                                 SERVO_VERTICAL_MAX,
                                 SERVO_VERTICAL_INVERT);
}

void hardware_reset_position(void)
{
    hardware_set_position_from_coord(START_X, START_Y);
}

static uint32_t servo_angle_to_us(int angle)
{
    if (angle < SERVO_PULSE_ANGLE_MIN) {
        angle = SERVO_PULSE_ANGLE_MIN;
    }
    if (angle > SERVO_PULSE_ANGLE_MAX) {
        angle = SERVO_PULSE_ANGLE_MAX;
    }

    return SERVO_MIN_US +
           ((SERVO_MAX_US - SERVO_MIN_US) * (uint32_t)(angle - SERVO_PULSE_ANGLE_MIN)) /
               (SERVO_PULSE_ANGLE_MAX - SERVO_PULSE_ANGLE_MIN);
}

static void gpio_output_init(uint8_t gpio)
{
    switch (gpio) {
    case 4:
        PIN_FUNC_SELECT(GPIO_PIN_REG_4, FUNC_GPIO4);
        break;
    case 5:
        PIN_FUNC_SELECT(GPIO_PIN_REG_5, FUNC_GPIO5);
        break;
    case 14:
        PIN_FUNC_SELECT(GPIO_PIN_REG_14, FUNC_GPIO14);
        break;
    default:
        break;
    }

    GPIO_AS_OUTPUT(BIT(gpio));
    GPIO_OUTPUT_SET(gpio, 0);
}

void hardware_laser_init_safe_state(void)
{
    gpio_output_init(LASER_GPIO);
    GPIO_OUTPUT_SET(LASER_GPIO, laser_gpio_level(false));
    laser_on = false;
    HARDWARE_LOG("laser safe init gpio=%d active_low=%d level=%d",
                 LASER_GPIO, LASER_ACTIVE_LOW, laser_gpio_level(false));
}

void hardware_laser_set(bool enabled)
{
    laser_on = enabled;
    GPIO_OUTPUT_SET(LASER_GPIO, laser_gpio_level(enabled));
    HARDWARE_LOG("laser gpio=%d requested=%s active_low=%d level=%d",
                 LASER_GPIO,
                 enabled ? "ON" : "OFF",
                 LASER_ACTIVE_LOW,
                 laser_gpio_level(enabled));
}

void hardware_laser_set_raw_level(bool high)
{
    laser_on = (high == laser_gpio_level(true));
    GPIO_OUTPUT_SET(LASER_GPIO, high ? 1 : 0);
    HARDWARE_LOG("laser raw gpio=%d level=%d logical=%s",
                 LASER_GPIO,
                 high ? 1 : 0,
                 laser_on ? "ON" : "OFF");
}

bool hardware_laser_is_on(void)
{
    return laser_on;
}

void hardware_servo_set_enabled(bool enabled)
{
    servo_enabled = enabled;
    if (!enabled) {
        GPIO_OUTPUT_SET(SERVO_HORIZONTAL_GPIO, 0);
        GPIO_OUTPUT_SET(SERVO_VERTICAL_GPIO, 0);
    }
}

void hardware_init(void)
{
    gpio_output_init(SERVO_HORIZONTAL_GPIO);
    gpio_output_init(SERVO_VERTICAL_GPIO);
    hardware_laser_init_safe_state();

    hardware_reset_position();
    hardware_servo_set_enabled(false);
    hardware_laser_set(false);
}

void hardware_servo_pwm_task(void *arg)
{
    (void)arg;

    portTickType last_wake = xTaskGetTickCount();

    while (true) {
        if (!servo_enabled) {
            GPIO_OUTPUT_SET(SERVO_HORIZONTAL_GPIO, 0);
            GPIO_OUTPUT_SET(SERVO_VERTICAL_GPIO, 0);
            vTaskDelayUntil(&last_wake, ms_to_ticks_min1(SERVO_PERIOD_MS));
            continue;
        }

        uint32_t h_us = servo_angle_to_us(servo_h_pos);
        uint32_t v_us = servo_angle_to_us(servo_v_pos);

        GPIO_OUTPUT_SET(SERVO_HORIZONTAL_GPIO, 1);
        os_delay_us((uint16)h_us);
        GPIO_OUTPUT_SET(SERVO_HORIZONTAL_GPIO, 0);

        GPIO_OUTPUT_SET(SERVO_VERTICAL_GPIO, 1);
        os_delay_us((uint16)v_us);
        GPIO_OUTPUT_SET(SERVO_VERTICAL_GPIO, 0);

        vTaskDelayUntil(&last_wake, ms_to_ticks_min1(SERVO_PERIOD_MS));
    }
}
