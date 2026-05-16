#include "game.h"

#include "app_util.h"
#include "default_patterns.h"
#include "hardware.h"
#include "logging.h"
#include "main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_common.h"
#include "freertos/task.h"

static volatile bool toy_enabled = false;
static int16_t current_x = START_X;
static int16_t current_y = START_Y;
static const pattern_pack_t *active_pack = &default_pattern_pack;
static char pattern_status[96] = "Pack compile actif.";
static volatile int16_t selected_pattern_index = -1;
static volatile uint16_t speed_percent = PATTERN_SPEED_DEFAULT_PERCENT;
static volatile uint32_t pattern_control_revision = 0;
static volatile uint32_t active_run_revision = 0;

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

void game_use_default_patterns(void)
{
    game_set_enabled(false);
    active_pack = &default_pattern_pack;
    selected_pattern_index = -1;
    pattern_control_revision++;
    snprintf(pattern_status, sizeof(pattern_status), "Pack compile actif.");
    PATTERN_LOG("pack reset source=%s count=%d revision=%u",
                active_pack->source_name,
                active_pack->pattern_count,
                (unsigned)pattern_control_revision);
}

bool game_use_pattern_pack(const pattern_pack_t *pack, const char *status)
{
    if (pack == NULL || pack->patterns == NULL || pack->pattern_count == 0) {
        snprintf(pattern_status, sizeof(pattern_status), "Pack invalide, fallback compile.");
        game_use_default_patterns();
        return false;
    }

    game_set_enabled(false);
    hardware_laser_set(false);
    active_pack = pack;
    selected_pattern_index = -1;
    pattern_control_revision++;
    snprintf(pattern_status, sizeof(pattern_status), "%s", status == NULL ? "Pack JSON actif." : status);
    PATTERN_LOG("pack active source=%s count=%d capture_every=%d revision=%u",
                active_pack->source_name,
                active_pack->pattern_count,
                active_pack->capture_every,
                (unsigned)pattern_control_revision);
    return true;
}

const pattern_pack_t *game_get_pattern_pack(void)
{
    return active_pack;
}

const char *game_get_pattern_status(void)
{
    return pattern_status;
}

bool game_set_selected_pattern(int16_t pattern_index)
{
    const pattern_pack_t *pack = active_pack == NULL ? &default_pattern_pack : active_pack;

    if (pattern_index < 0) {
        selected_pattern_index = -1;
        pattern_control_revision++;
        PATTERN_LOG("selection changed mode=auto revision=%u",
                    (unsigned)pattern_control_revision);
        return true;
    }
    if (pattern_index >= (int16_t)pack->pattern_count) {
        PATTERN_LOG("selection rejected index=%d count=%d current=%d revision=%u",
                    pattern_index,
                    pack->pattern_count,
                    selected_pattern_index,
                    (unsigned)pattern_control_revision);
        return false;
    }

    selected_pattern_index = pattern_index;
    pattern_control_revision++;
    PATTERN_LOG("selection changed index=%d id=%s revision=%u",
                pattern_index,
                pack->patterns[pattern_index].id,
                (unsigned)pattern_control_revision);
    return true;
}

int16_t game_get_selected_pattern(void)
{
    return selected_pattern_index;
}

const char *game_get_selected_pattern_id(void)
{
    const pattern_pack_t *pack = active_pack == NULL ? &default_pattern_pack : active_pack;
    int16_t index = selected_pattern_index;

    if (index < 0 || index >= (int16_t)pack->pattern_count) {
        return "auto";
    }
    return pack->patterns[index].id;
}

void game_set_speed_percent(uint16_t new_speed_percent)
{
    uint16_t requested_speed_percent = new_speed_percent;

    if (new_speed_percent < PATTERN_SPEED_MIN_PERCENT) {
        new_speed_percent = PATTERN_SPEED_MIN_PERCENT;
    }
    if (new_speed_percent > PATTERN_SPEED_MAX_PERCENT) {
        new_speed_percent = PATTERN_SPEED_MAX_PERCENT;
    }

    speed_percent = new_speed_percent;
    pattern_control_revision++;
    PATTERN_LOG("speed changed requested=%u applied=%u revision=%u",
                requested_speed_percent,
                speed_percent,
                (unsigned)pattern_control_revision);
}

uint16_t game_get_speed_percent(void)
{
    return speed_percent;
}

void game_set_enabled(bool enabled)
{
    toy_enabled = enabled;
    if (enabled) {
        hardware_servo_set_enabled(true);
    } else {
        hardware_laser_set(false);
        hardware_reset_position();
        hardware_servo_set_enabled(false);
        current_x = START_X;
        current_y = START_Y;
    }
}

bool game_is_enabled(void)
{
    return toy_enabled;
}

static int random_range(int min_inclusive, int max_exclusive)
{
    if (max_exclusive <= min_inclusive) {
        return min_inclusive;
    }

    return min_inclusive + (rand() % (max_exclusive - min_inclusive));
}

static bool wait_enabled_delay(uint32_t duration_ms)
{
    uint32_t elapsed = 0;

    while (elapsed < duration_ms) {
        if (!toy_enabled) {
            game_set_enabled(false);
            return false;
        }
        if (pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run interrupted during wait active_revision=%u current_revision=%u",
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision);
            return false;
        }

        uint32_t delay = MOTION_TICK_MS;
        if (elapsed + delay > duration_ms) {
            delay = duration_ms - elapsed;
        }

        vTaskDelay(ms_to_ticks_min1(delay));
        elapsed += delay;
    }

    return true;
}

static uint32_t scaled_duration(uint32_t duration_ms)
{
    uint16_t speed = speed_percent;
    if (speed < PATTERN_SPEED_MIN_PERCENT) {
        speed = PATTERN_SPEED_MIN_PERCENT;
    }

    uint32_t scaled = (duration_ms * 100u) / speed;
    if (scaled < MOTION_TICK_MS) {
        scaled = MOTION_TICK_MS;
    }
    return scaled;
}

static uint16_t smoothstep_q10(uint32_t elapsed_ms, uint32_t duration_ms)
{
    if (duration_ms == 0 || elapsed_ms >= duration_ms) {
        return 1024;
    }

    uint32_t p = (elapsed_ms * 1024u) / duration_ms;
    if (p > 1024u) {
        p = 1024u;
    }

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

static void set_game_position(int16_t x, int16_t y)
{
    current_x = clamp_coord(x);
    current_y = clamp_coord(y);
    hardware_set_position_from_coord(current_x, current_y);
}

static const pattern_t *choose_weighted_pattern(void)
{
    uint16_t total = 0;
    const pattern_pack_t *pack = active_pack == NULL ? &default_pattern_pack : active_pack;

    for (uint8_t i = 0; i < pack->pattern_count; i++) {
        total += pack->patterns[i].weight;
    }

    if (total == 0) {
        return &pack->patterns[0];
    }

    uint16_t draw = (uint16_t)random_range(0, total);

    for (uint8_t i = 0; i < pack->pattern_count; i++) {
        if (draw < pack->patterns[i].weight) {
            return &pack->patterns[i];
        }
        draw -= pack->patterns[i].weight;
    }

    return &pack->patterns[0];
}

static const pattern_t *find_capture_pattern(const pattern_pack_t *pack)
{
    if (pack == NULL || pack == &default_pattern_pack) {
        return &capture_pattern;
    }

    for (uint8_t i = 0; i < pack->pattern_count; i++) {
        if (strcmp(pack->patterns[i].id, "capture") == 0 ||
            strcmp(pack->patterns[i].id, "final_wind_down") == 0 ||
            strcmp(pack->patterns[i].id, "low_capture_zone") == 0) {
            return &pack->patterns[i];
        }
    }

    return NULL;
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
    for (uint16_t i = 0; i < pattern->step_count; i++) {
        if (step_has_position(&pattern->steps[i])) {
            return &pattern->steps[i];
        }
    }

    return NULL;
}

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
    uint16_t duration = (uint16_t)scaled_duration((uint32_t)random_range(PATTERN_TRANSITION_MIN_MS, PATTERN_TRANSITION_MAX_MS + 1));

    hardware_laser_set(false);

    for (uint32_t elapsed = 0; elapsed <= duration; elapsed += MOTION_TICK_MS) {
        if (!toy_enabled) {
            return;
        }
        if (pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run interrupted during transition active_revision=%u current_revision=%u",
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision);
            return;
        }

        int16_t x = interp_coord(from_x, to_x, elapsed, duration);
        int16_t y = interp_coord(from_y, to_y, elapsed, duration);
        set_game_position(x, y);
        vTaskDelay(ms_to_ticks_min1(MOTION_TICK_MS));
    }

    set_game_position(to_x, to_y);
}

static void run_hold_step(const pattern_step_t *step)
{
    if (step_has_position(step)) {
        set_game_position(step->x, step->y);
    }

    hardware_laser_set(step->laser);
    wait_enabled_delay(scaled_duration(step->duration_ms));
}

static void run_move_step(const pattern_step_t *step)
{
    int16_t from_x = current_x;
    int16_t from_y = current_y;
    int16_t to_x = step->x;
    int16_t to_y = step->y;
    uint32_t duration = scaled_duration(step->duration_ms);

    hardware_laser_set(step->laser);

    for (uint32_t elapsed = 0; elapsed <= duration; elapsed += MOTION_TICK_MS) {
        if (!toy_enabled) {
            return;
        }
        if (pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run interrupted during move active_revision=%u current_revision=%u",
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision);
            return;
        }

        int16_t x = interp_coord(from_x, to_x, elapsed, duration);
        int16_t y = interp_coord(from_y, to_y, elapsed, duration);
        set_game_position(x, y);
        vTaskDelay(ms_to_ticks_min1(MOTION_TICK_MS));
    }

    set_game_position(to_x, to_y);
}

static void run_jitter_step(const pattern_step_t *step)
{
    uint32_t elapsed_total = 0;
    uint32_t duration = scaled_duration(step->duration_ms);
    uint32_t jitter_interval = scaled_duration(JITTER_POINT_INTERVAL_MS);
    int16_t center_x = step->x;
    int16_t center_y = step->y;

    hardware_laser_set(step->laser);

    while (elapsed_total < duration) {
        if (!toy_enabled) {
            return;
        }
        if (pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run interrupted during jitter active_revision=%u current_revision=%u",
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision);
            return;
        }

        int16_t from_x = current_x;
        int16_t from_y = current_y;
        int16_t dx = (int16_t)random_range(-(int)step->amplitude, (int)step->amplitude + 1);
        int16_t dy = (int16_t)random_range(-(int)step->amplitude, (int)step->amplitude + 1);
        int16_t to_x = clamp_coord((int32_t)center_x + dx);
        int16_t to_y = clamp_coord((int32_t)center_y + dy);

        uint32_t segment = jitter_interval;
        if (elapsed_total + segment > duration) {
            segment = duration - elapsed_total;
        }

        for (uint32_t elapsed = 0; elapsed <= segment; elapsed += MOTION_TICK_MS) {
            if (!toy_enabled) {
                return;
            }
            if (pattern_control_revision != active_run_revision) {
                PATTERN_LOG("run interrupted during jitter move active_revision=%u current_revision=%u",
                            (unsigned)active_run_revision,
                            (unsigned)pattern_control_revision);
                return;
            }

            int16_t x = interp_coord(from_x, to_x, elapsed, segment);
            int16_t y = interp_coord(from_y, to_y, elapsed, segment);
            set_game_position(x, y);
            vTaskDelay(ms_to_ticks_min1(MOTION_TICK_MS));
        }

        elapsed_total += segment;
    }

    set_game_position(center_x, center_y);
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
        hardware_laser_set(false);
        wait_enabled_delay(scaled_duration(step->duration_ms));
        break;
    case STEP_OFF_MOVE:
        run_move_step(step);
        break;
    default:
        hardware_laser_set(false);
        break;
    }
}

static void run_pattern(const pattern_t *pattern)
{
    PATTERN_LOG("run start id=%s name=%s steps=%u speed=%u selected=%d revision=%u",
                pattern->id,
                pattern->name,
                pattern->step_count,
                speed_percent,
                selected_pattern_index,
                (unsigned)active_run_revision);

    prepare_pattern_start(pattern);

    for (uint16_t i = 0; i < pattern->step_count; i++) {
        if (!toy_enabled || pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run stop id=%s at_step=%u active_revision=%u current_revision=%u enabled=%d",
                        pattern->id,
                        i,
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision,
                        toy_enabled);
            return;
        }
        run_step(&pattern->steps[i]);
    }

    PATTERN_LOG("run done id=%s revision=%u", pattern->id, (unsigned)active_run_revision);
}

void game_movement_task(void *arg)
{
    (void)arg;

    uint8_t since_capture = 0;

    while (true) {
        if (!toy_enabled) {
            vTaskDelay(ms_to_ticks_min1(200));
            continue;
        }

        const pattern_t *pattern;
        const pattern_pack_t *pack = active_pack == NULL ? &default_pattern_pack : active_pack;
        uint8_t capture_every = pack->capture_every == 0 ? PATTERN_CAPTURE_EVERY : pack->capture_every;
        const pattern_t *capture = find_capture_pattern(pack);

        int16_t selected = selected_pattern_index;
        active_run_revision = pattern_control_revision;
        if (selected >= 0 && selected < (int16_t)pack->pattern_count) {
            pattern = &pack->patterns[selected];
            PATTERN_LOG("choose reason=manual index=%d id=%s speed=%u revision=%u",
                        selected,
                        pattern->id,
                        speed_percent,
                        (unsigned)active_run_revision);
        } else if (capture != NULL && since_capture >= capture_every) {
            pattern = capture;
            since_capture = 0;
            PATTERN_LOG("choose reason=capture id=%s speed=%u revision=%u",
                        pattern->id,
                        speed_percent,
                        (unsigned)active_run_revision);
        } else {
            pattern = choose_weighted_pattern();
            since_capture++;
            PATTERN_LOG("choose reason=weighted id=%s since_capture=%u/%u speed=%u revision=%u",
                        pattern->id,
                        since_capture,
                        capture_every,
                        speed_percent,
                        (unsigned)active_run_revision);
        }

        run_pattern(pattern);

        hardware_laser_set(false);
        wait_enabled_delay(scaled_duration((uint32_t)random_range(800, 1800)));
    }
}
