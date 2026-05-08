// Laser Cat Toy - ESP8266 RTOS SDK
// Version avec moteur de patterns coordonnés.
//
// Principe :
// - plus de mouvements aléatoires indépendants par axe ;
// - chaque scène suit un pattern structuré ;
// - les deux servos sont interpolés ensemble ;
// - le laser peut disparaître pendant les transitions invisibles ;
// - les trajectoires simulent davantage une proie qu'un simple balayage.

#include "main.h"

#include <stdlib.h>

#include "esp_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio.h"
#include "uart.h"

// -----------------------------------------------------------------------------
// Types patterns
// -----------------------------------------------------------------------------

typedef enum {
    STEP_HOLD,
    STEP_MOVE,
    STEP_JITTER,
    STEP_OFF_HOLD,
    STEP_OFF_MOVE
} pattern_step_type_t;

typedef struct {
    pattern_step_type_t type;
    bool laser;
    int16_t x;
    int16_t y;
    uint16_t duration_ms;
    uint16_t amplitude;
} pattern_step_t;

typedef struct {
    const char *id;
    const char *name;
    uint8_t weight;
    uint8_t step_count;
    const pattern_step_t *steps;
} pattern_t;

#define ARRAY_SIZE(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

// -----------------------------------------------------------------------------
// Etat global
// -----------------------------------------------------------------------------

static volatile int servo_h_pos = 90;
static volatile int servo_v_pos = 90;
static volatile bool laser_on = false;

static int16_t current_x = START_X;
static int16_t current_y = START_Y;

// -----------------------------------------------------------------------------
// Patterns embarqués
// -----------------------------------------------------------------------------

static const pattern_step_t pattern_mouse_cautious[] = {
    { STEP_HOLD,     true,  -400, -200,  800,  0 },
    { STEP_MOVE,     true,  -100, -200, 1800,  0 },
    { STEP_HOLD,     true,  -100, -200,  900,  0 },
    { STEP_JITTER,   true,  -100, -200,  900, 30 },
    { STEP_MOVE,     true,   200, -150, 1600,  0 },
    { STEP_OFF_HOLD, false,    0,    0,  500,  0 },
    { STEP_OFF_MOVE, false,  350, -100,  400,  0 },
    { STEP_HOLD,     true,   350, -100, 1200,  0 }
};

static const pattern_step_t pattern_insect_nervous[] = {
    { STEP_HOLD,     true,     0,    0,  500,  0 },
    { STEP_MOVE,     true,   150,   50,  500,  0 },
    { STEP_MOVE,     true,    50,  150,  450,  0 },
    { STEP_MOVE,     true,   220,  100,  500,  0 },
    { STEP_HOLD,     true,   220,  100,  400,  0 },
    { STEP_MOVE,     true,   100,  -50,  600,  0 },
    { STEP_JITTER,   true,   100,  -50,  700, 40 },
    { STEP_OFF_HOLD, false,    0,    0,  300,  0 },
    { STEP_HOLD,     true,   180,  -80,  900,  0 }
};

static const pattern_step_t pattern_escape_to_edge[] = {
    { STEP_HOLD,     true,  -200,    0,  900,  0 },
    { STEP_JITTER,   true,  -200,    0,  700, 30 },
    { STEP_MOVE,     true,  -650,   50,  700,  0 },
    { STEP_HOLD,     true,  -650,   50, 1200,  0 },
    { STEP_OFF_HOLD, false,    0,    0,  600,  0 },
    { STEP_OFF_MOVE, false, -750,  250,  500,  0 },
    { STEP_HOLD,     true,  -750,  250, 1000,  0 }
};

static const pattern_step_t pattern_hide_and_seek[] = {
    { STEP_MOVE,     true,   100, -300, 1200,  0 },
    { STEP_HOLD,     true,   100, -300,  600,  0 },
    { STEP_MOVE,     true,   350, -300,  800,  0 },
    { STEP_OFF_HOLD, false,    0,    0, 1000,  0 },
    { STEP_OFF_MOVE, false,  450, -200,  500,  0 },
    { STEP_HOLD,     true,   450, -200,  600,  0 },
    { STEP_MOVE,     true,   300, -100,  900,  0 },
    { STEP_OFF_HOLD, false,    0,    0,  500,  0 }
};

static const pattern_step_t pattern_rectangle_patrol[] = {
    { STEP_HOLD,     true,  -500, -400,  800,  0 },
    { STEP_MOVE,     true,   200, -400, 2200,  0 },
    { STEP_HOLD,     true,   200, -400, 1000,  0 },
    { STEP_MOVE,     true,   250, -150, 1200,  0 },
    { STEP_HOLD,     true,   250, -150,  700,  0 },
    { STEP_MOVE,     true,  -400, -150, 2000,  0 },
    { STEP_OFF_HOLD, false,    0,    0,  400,  0 },
    { STEP_OFF_MOVE, false, -500, -400,  600,  0 },
    { STEP_HOLD,     true,  -500, -400, 1200,  0 }
};

static const pattern_step_t pattern_triangle_ambush[] = {
    { STEP_HOLD,     true,     0, -350,  900,  0 },
    { STEP_MOVE,     true,   350,   50, 1600,  0 },
    { STEP_HOLD,     true,   350,   50, 1000,  0 },
    { STEP_JITTER,   true,   350,   50,  600, 20 },
    { STEP_MOVE,     true,  -250,  150, 1800,  0 },
    { STEP_HOLD,     true,  -250,  150,  800,  0 },
    { STEP_OFF_MOVE, false,    0, -350,  700,  0 },
    { STEP_HOLD,     true,     0, -350, 1200,  0 }
};

static const pattern_step_t pattern_broken_ellipse[] = {
    { STEP_HOLD,     true,     0,    0,  700,  0 },
    { STEP_MOVE,     true,   250,   50, 1000,  0 },
    { STEP_MOVE,     true,   350,  200, 1000,  0 },
    { STEP_MOVE,     true,   150,  350, 1000,  0 },
    { STEP_HOLD,     true,   150,  350,  900,  0 },
    { STEP_OFF_HOLD, false,    0,    0,  400,  0 },
    { STEP_OFF_MOVE, false, -100,  250,  500,  0 },
    { STEP_MOVE,     true,  -300,   50, 1200,  0 },
    { STEP_HOLD,     true,  -300,   50,  900,  0 }
};

static const pattern_step_t pattern_radial_star_escape[] = {
    { STEP_HOLD,     true,     0,    0,  900,  0 },
    { STEP_MOVE,     true,   350,    0,  600,  0 },
    { STEP_OFF_MOVE, false,    0,    0,  300,  0 },
    { STEP_MOVE,     true,  -250,  200,  650,  0 },
    { STEP_OFF_MOVE, false,    0,    0,  300,  0 },
    { STEP_MOVE,     true,   100, -350,  700,  0 },
    { STEP_OFF_MOVE, false,    0,    0,  300,  0 },
    { STEP_HOLD,     true,     0,    0, 1000,  0 }
};

static const pattern_step_t pattern_broken_hexagon[] = {
    { STEP_HOLD,     true,  -300, -200,  800,  0 },
    { STEP_MOVE,     true,     0, -300, 1300,  0 },
    { STEP_MOVE,     true,   300, -150, 1300,  0 },
    { STEP_HOLD,     true,   300, -150,  900,  0 },
    { STEP_MOVE,     true,   250,  200, 1300,  0 },
    { STEP_OFF_HOLD, false,    0,    0,  500,  0 },
    { STEP_OFF_MOVE, false, -150,  250,  500,  0 },
    { STEP_HOLD,     true,  -150,  250,  900,  0 },
    { STEP_MOVE,     true,  -350,    0, 1400,  0 }
};

static const pattern_step_t pattern_capture[] = {
    { STEP_MOVE,     true,   100, -100, 1200,  0 },
    { STEP_MOVE,     true,     0, -350, 1600,  0 },
    { STEP_HOLD,     true,     0, -350, 2500,  0 },
    { STEP_JITTER,   true,     0, -350, 1200, 15 },
    { STEP_OFF_HOLD, false,    0,    0, 2000,  0 }
};

static const pattern_t weighted_patterns[] = {
    { "mouse_cautious",       "Souris prudente",                  35, ARRAY_SIZE(pattern_mouse_cautious),      pattern_mouse_cautious },
    { "escape_to_edge",       "Fuite vers un bord",               20, ARRAY_SIZE(pattern_escape_to_edge),      pattern_escape_to_edge },
    { "hide_and_seek",        "Cache-cache derrière objet",       15, ARRAY_SIZE(pattern_hide_and_seek),       pattern_hide_and_seek },
    { "insect_nervous",       "Insecte nerveux",                  10, ARRAY_SIZE(pattern_insect_nervous),      pattern_insect_nervous },
    { "rectangle_patrol",     "Rectangle patrouille",              7, ARRAY_SIZE(pattern_rectangle_patrol),    pattern_rectangle_patrol },
    { "triangle_ambush",      "Triangle d'embuscade",              7, ARRAY_SIZE(pattern_triangle_ambush),     pattern_triangle_ambush },
    { "broken_ellipse",       "Ellipse cassée",                    5, ARRAY_SIZE(pattern_broken_ellipse),      pattern_broken_ellipse },
    { "radial_star_escape",   "Étoile radiale",                    3, ARRAY_SIZE(pattern_radial_star_escape),  pattern_radial_star_escape },
    { "broken_hexagon",       "Hexagone lent cassé",               2, ARRAY_SIZE(pattern_broken_hexagon),      pattern_broken_hexagon }
};

static const pattern_t capture_pattern = {
    "capture",
    "Capture",
    0,
    ARRAY_SIZE(pattern_capture),
    pattern_capture
};

// -----------------------------------------------------------------------------
// Utilitaires temps / hasard
// -----------------------------------------------------------------------------

static portTickType ms_to_ticks_min1(uint32_t ms)
{
    portTickType ticks = (portTickType)((ms + portTICK_RATE_MS - 1) / portTICK_RATE_MS);
    return ticks == 0 ? 1 : ticks;
}

static int random_range(int min_inclusive, int max_exclusive)
{
    if (max_exclusive <= min_inclusive) {
        return min_inclusive;
    }

    return min_inclusive + (rand() % (max_exclusive - min_inclusive));
}

static int16_t clamp_coord(int32_t v)
{
    if (v < COORD_MIN) {
        return COORD_MIN;
    }
    if (v > COORD_MAX) {
        return COORD_MAX;
    }
    return (int16_t)v;
}

// Smoothstep Q10 : entrée 0..duration, sortie 0..1024.
static uint16_t smoothstep_q10(uint32_t elapsed_ms, uint32_t duration_ms)
{
    if (duration_ms == 0 || elapsed_ms >= duration_ms) {
        return 1024;
    }

    uint32_t p = (elapsed_ms * 1024u) / duration_ms;
    if (p > 1024u) {
        p = 1024u;
    }

    // s = p² * (3 - 2p), en Q10.
    uint64_t pp = (uint64_t)p * (uint64_t)p;
    uint64_t s = (pp * (uint64_t)(3072u - (2u * p))) / (1024u * 1024u);

    if (s > 1024u) {
        s = 1024u;
    }
    return (uint16_t)s;
}

static int16_t interp_coord(int16_t from, int16_t to, uint32_t elapsed_ms, uint32_t duration_ms)
{
    uint16_t s = smoothstep_q10(elapsed_ms, duration_ms);
    int32_t delta = (int32_t)to - (int32_t)from;
    return clamp_coord((int32_t)from + ((delta * (int32_t)s) / 1024));
}

// -----------------------------------------------------------------------------
// Mapping coordonnées -> angles servo
// -----------------------------------------------------------------------------

static int coord_to_angle(int16_t coord, int min_angle, int max_angle, int invert)
{
    coord = clamp_coord(coord);

    if (invert) {
        coord = (int16_t)-coord;
    }

    int32_t span = (int32_t)max_angle - (int32_t)min_angle;
    int32_t v = (int32_t)(coord + 1000); // 0..2000
    int32_t angle = (int32_t)min_angle + ((v * span) / 2000);

    if (angle < min_angle) {
        angle = min_angle;
    }
    if (angle > max_angle) {
        angle = max_angle;
    }

    return (int)angle;
}

static void set_position_from_coord(int16_t x, int16_t y)
{
    current_x = clamp_coord(x);
    current_y = clamp_coord(y);

    servo_h_pos = coord_to_angle(current_x,
                                 SERVO_HORIZONTAL_MIN,
                                 SERVO_HORIZONTAL_MAX,
                                 SERVO_HORIZONTAL_INVERT);

    servo_v_pos = coord_to_angle(current_y,
                                 SERVO_VERTICAL_MIN,
                                 SERVO_VERTICAL_MAX,
                                 SERVO_VERTICAL_INVERT);
}

static uint32_t servo_angle_to_us(int angle)
{
    if (angle < 0) {
        angle = 0;
    }
    if (angle > 180) {
        angle = 180;
    }

    return SERVO_MIN_US + ((SERVO_MAX_US - SERVO_MIN_US) * (uint32_t)angle) / 180;
}

// -----------------------------------------------------------------------------
// GPIO
// -----------------------------------------------------------------------------

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

static void laser_set(bool enabled)
{
    laser_on = enabled;
    GPIO_OUTPUT_SET(LASER_GPIO, enabled ? 1 : 0);
}

// -----------------------------------------------------------------------------
// Sélection de pattern
// -----------------------------------------------------------------------------

static const pattern_t *choose_weighted_pattern(void)
{
    uint16_t total = 0;

    for (uint8_t i = 0; i < ARRAY_SIZE(weighted_patterns); i++) {
        total += weighted_patterns[i].weight;
    }

    if (total == 0) {
        return &weighted_patterns[0];
    }

    uint16_t draw = (uint16_t)random_range(0, total);

    for (uint8_t i = 0; i < ARRAY_SIZE(weighted_patterns); i++) {
        if (draw < weighted_patterns[i].weight) {
            return &weighted_patterns[i];
        }
        draw -= weighted_patterns[i].weight;
    }

    return &weighted_patterns[0];
}

static bool step_has_position(const pattern_step_t *step)
{
    return step->type == STEP_HOLD ||
           step->type == STEP_MOVE ||
           step->type == STEP_JITTER ||
           step->type == STEP_OFF_MOVE;
}

static const pattern_step_t *first_position_step(const pattern_t *pattern)
{
    for (uint8_t i = 0; i < pattern->step_count; i++) {
        if (step_has_position(&pattern->steps[i])) {
            return &pattern->steps[i];
        }
    }

    return NULL;
}

// Transition invisible vers le premier point du nouveau pattern.
// Évite les sauts visibles du laser entre deux scènes.
static void prepare_pattern_start(const pattern_t *pattern)
{
    const pattern_step_t *first = first_position_step(pattern);
    if (first == NULL) {
        return;
    }

    int16_t from_x = current_x;
    int16_t from_y = current_y;
    int16_t to_x = first->x;
    int16_t to_y = first->y;
    uint16_t duration = (uint16_t)random_range(PATTERN_TRANSITION_MIN_MS, PATTERN_TRANSITION_MAX_MS + 1);

    laser_set(false);

    for (uint32_t elapsed = 0; elapsed <= duration; elapsed += MOTION_TICK_MS) {
        int16_t x = interp_coord(from_x, to_x, elapsed, duration);
        int16_t y = interp_coord(from_y, to_y, elapsed, duration);
        set_position_from_coord(x, y);
        vTaskDelay(ms_to_ticks_min1(MOTION_TICK_MS));
    }

    set_position_from_coord(to_x, to_y);
}

// -----------------------------------------------------------------------------
// Exécution des steps
// -----------------------------------------------------------------------------

static void run_hold_step(const pattern_step_t *step)
{
    if (step_has_position(step)) {
        set_position_from_coord(step->x, step->y);
    }

    laser_set(step->laser);
    vTaskDelay(ms_to_ticks_min1(step->duration_ms));
}

static void run_move_step(const pattern_step_t *step)
{
    int16_t from_x = current_x;
    int16_t from_y = current_y;
    int16_t to_x = step->x;
    int16_t to_y = step->y;

    laser_set(step->laser);

    for (uint32_t elapsed = 0; elapsed <= step->duration_ms; elapsed += MOTION_TICK_MS) {
        int16_t x = interp_coord(from_x, to_x, elapsed, step->duration_ms);
        int16_t y = interp_coord(from_y, to_y, elapsed, step->duration_ms);
        set_position_from_coord(x, y);
        vTaskDelay(ms_to_ticks_min1(MOTION_TICK_MS));
    }

    set_position_from_coord(to_x, to_y);
}

static void run_jitter_step(const pattern_step_t *step)
{
    uint32_t elapsed_total = 0;
    int16_t center_x = step->x;
    int16_t center_y = step->y;
    int16_t from_x = current_x;
    int16_t from_y = current_y;
    int16_t to_x = center_x;
    int16_t to_y = center_y;

    laser_set(step->laser);

    while (elapsed_total < step->duration_ms) {
        from_x = current_x;
        from_y = current_y;

        int16_t dx = (int16_t)random_range(-(int)step->amplitude, (int)step->amplitude + 1);
        int16_t dy = (int16_t)random_range(-(int)step->amplitude, (int)step->amplitude + 1);

        to_x = clamp_coord((int32_t)center_x + dx);
        to_y = clamp_coord((int32_t)center_y + dy);

        uint32_t segment = JITTER_POINT_INTERVAL_MS;
        if (elapsed_total + segment > step->duration_ms) {
            segment = step->duration_ms - elapsed_total;
        }

        for (uint32_t elapsed = 0; elapsed <= segment; elapsed += MOTION_TICK_MS) {
            int16_t x = interp_coord(from_x, to_x, elapsed, segment);
            int16_t y = interp_coord(from_y, to_y, elapsed, segment);
            set_position_from_coord(x, y);
            vTaskDelay(ms_to_ticks_min1(MOTION_TICK_MS));
        }

        elapsed_total += segment;
    }

    set_position_from_coord(center_x, center_y);
}

static void run_step(const pattern_step_t *step)
{
    switch (step->type) {
    case STEP_HOLD:
        run_hold_step(step);
        break;

    case STEP_MOVE:
        run_move_step(step);
        break;

    case STEP_JITTER:
        run_jitter_step(step);
        break;

    case STEP_OFF_HOLD:
        laser_set(false);
        vTaskDelay(ms_to_ticks_min1(step->duration_ms));
        break;

    case STEP_OFF_MOVE:
        run_move_step(step);
        break;

    default:
        laser_set(false);
        break;
    }
}

static void run_pattern(const pattern_t *pattern)
{
    os_printf("pattern: %s\n", pattern->id);

    prepare_pattern_start(pattern);

    for (uint8_t i = 0; i < pattern->step_count; i++) {
        run_step(&pattern->steps[i]);
    }
}

// -----------------------------------------------------------------------------
// Tâche mouvement
// -----------------------------------------------------------------------------

static void movement_task(void *arg)
{
    (void)arg;

    uint8_t since_capture = 0;

    while (true) {
        const pattern_t *pattern;

        if (since_capture >= PATTERN_CAPTURE_EVERY) {
            pattern = &capture_pattern;
            since_capture = 0;
        } else {
            pattern = choose_weighted_pattern();
            since_capture++;
        }

        run_pattern(pattern);

        // Respiration entre deux scènes : laser éteint, courte pause.
        laser_set(false);
        vTaskDelay(ms_to_ticks_min1((uint32_t)random_range(800, 1800)));
    }
}

// -----------------------------------------------------------------------------
// Tâche PWM servo : 50 Hz
// -----------------------------------------------------------------------------

static void servo_pwm_task(void *arg)
{
    (void)arg;

    portTickType last_wake = xTaskGetTickCount();

    while (true) {
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

// -----------------------------------------------------------------------------
// RF calibration ESP8266
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Entrée ESP8266 RTOS SDK
// -----------------------------------------------------------------------------

void user_init(void)
{
    UART_SetBaudrate(UART0, SERIAL_BAUD_RATE);

    srand((unsigned)(0x8266u ^ (uint32_t)xTaskGetTickCount()));

    gpio_output_init(SERVO_HORIZONTAL_GPIO);
    gpio_output_init(SERVO_VERTICAL_GPIO);
    gpio_output_init(LASER_GPIO);

    set_position_from_coord(START_X, START_Y);
    laser_set(false);

    os_printf("Laser Cat Toy ESP8266 RTOS start - pattern engine\n");
    os_printf("servo_h_gpio=%d servo_v_gpio=%d laser_gpio=%d\n",
              SERVO_HORIZONTAL_GPIO, SERVO_VERTICAL_GPIO, LASER_GPIO);

    xTaskCreate(servo_pwm_task, "servo_pwm", 384, NULL, 5, NULL);
    xTaskCreate(movement_task, "movement", 768, NULL, 3, NULL);
}