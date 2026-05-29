#include "game.h"

#include "app_util.h"
#include "default_patterns.h"
#include "hardware.h"
#include "logging.h"
#include "main.h"
#include "pattern_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_common.h"
#include "freertos/task.h"

static volatile bool toy_enabled = false;
static volatile game_state_t game_state = GAME_STATE_IDLE;
static int16_t current_x = START_X;
static int16_t current_y = START_Y;
static const pattern_pack_t *active_pack = &default_pattern_pack;
static char pattern_status[96] = "Pack compile actif.";
static volatile int16_t selected_pattern_index = -1;
static volatile uint16_t speed_percent = PATTERN_SPEED_DEFAULT_PERCENT;
static volatile uint32_t pattern_control_revision = 0;
static volatile uint32_t active_run_revision = 0;
static volatile uint32_t session_started_ms = 0;
static volatile uint32_t cooldown_until_ms = 0;
static volatile uint32_t laser_pulse_until_ms = 0;
static volatile uint32_t session_max_ms = GAME_SESSION_MAX_MS;
static volatile uint32_t cooldown_ms = GAME_COOLDOWN_MS;
static volatile uint16_t motion_scale_percent = 100;

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_RATE_MS;
}

static bool time_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static bool cooldown_is_active(void)
{
    uint32_t now = now_ms();
    return cooldown_until_ms != 0 && time_before(now, cooldown_until_ms);
}

static void stop_laser_pulse(void)
{
    laser_pulse_until_ms = 0;
    if (game_state == GAME_STATE_DEBUG) {
        hardware_laser_set(false);
        game_state = cooldown_is_active() ? GAME_STATE_COOLDOWN : GAME_STATE_IDLE;
    }
}

static void stop_session(bool start_cooldown)
{
    toy_enabled = false;
    pattern_control_revision++;
    hardware_laser_set(false);
    hardware_reset_position();
    hardware_servo_set_enabled(true);
    vTaskDelay(ms_to_ticks_min1(SERVO_REST_SETTLE_MS));
    hardware_servo_set_enabled(false);
    current_x = START_X;
    current_y = START_Y;
    session_started_ms = 0;

    if (start_cooldown) {
        cooldown_until_ms = now_ms() + cooldown_ms;
        game_state = GAME_STATE_COOLDOWN;
        PATTERN_LOG("session stopped cooldown_ms=%u revision=%u",
                    (unsigned)cooldown_ms,
                    (unsigned)pattern_control_revision);
    } else {
        cooldown_until_ms = 0;
        game_state = GAME_STATE_IDLE;
        PATTERN_LOG("session stopped manual revision=%u",
                    (unsigned)pattern_control_revision);
    }
}

static bool session_has_expired(void)
{
    if (!toy_enabled || session_started_ms == 0) {
        return false;
    }
    return now_ms() - session_started_ms >= session_max_ms;
}

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
    int16_t selected_index = selected_pattern_index;

    if (selected_index < 0 || selected_index >= (int16_t)pack->pattern_count) {
        return "auto";
    }
    return pack->patterns[selected_index].id;
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

bool game_set_enabled(bool enabled)
{
    if (enabled) {
        if (cooldown_is_active()) {
            game_state = GAME_STATE_COOLDOWN;
            PATTERN_LOG("session start rejected cooldown_remaining_ms=%u",
                        (unsigned)game_get_cooldown_remaining_ms());
            return false;
        }

        stop_laser_pulse();
        toy_enabled = true;
        game_state = GAME_STATE_RUNNING;
        session_started_ms = now_ms();
        cooldown_until_ms = 0;
        pattern_control_revision++;
        hardware_servo_set_enabled(true);
        hardware_laser_set(true);
        PATTERN_LOG("session started max_ms=%u revision=%u",
                    (unsigned)session_max_ms,
                    (unsigned)pattern_control_revision);
    } else {
        stop_laser_pulse();
        if (toy_enabled || game_state != GAME_STATE_IDLE) {
            stop_session(false);
        } else {
            hardware_laser_set(false);
            hardware_reset_position();
            hardware_servo_set_enabled(false);
        }
    }

    return true;
}

bool game_is_enabled(void)
{
    return toy_enabled;
}

game_state_t game_get_state(void)
{
    if (game_state == GAME_STATE_COOLDOWN && !cooldown_is_active()) {
        game_state = GAME_STATE_IDLE;
        cooldown_until_ms = 0;
    }
    return game_state;
}

const char *game_get_state_text(void)
{
    switch (game_get_state()) {
    case GAME_STATE_RUNNING:
        return "running";
    case GAME_STATE_COOLDOWN:
        return "cooldown";
    case GAME_STATE_DEBUG:
        return "debug";
    case GAME_STATE_IDLE:
    default:
        return "idle";
    }
}

uint32_t game_get_cooldown_remaining_ms(void)
{
    uint32_t now = now_ms();
    if (cooldown_until_ms == 0 || !time_before(now, cooldown_until_ms)) {
        return 0;
    }
    return cooldown_until_ms - now;
}

uint32_t game_get_session_remaining_ms(void)
{
    if (!toy_enabled || session_started_ms == 0) {
        return 0;
    }

    uint32_t elapsed = now_ms() - session_started_ms;
    if (elapsed >= session_max_ms) {
        return 0;
    }
    return session_max_ms - elapsed;
}

void game_set_session_max_ms(uint32_t new_session_max_ms)
{
    if (new_session_max_ms < 60u * 1000u) {
        new_session_max_ms = 60u * 1000u;
    }
    if (new_session_max_ms > 60u * 60u * 1000u) {
        new_session_max_ms = 60u * 60u * 1000u;
    }
    session_max_ms = new_session_max_ms;
}

uint32_t game_get_session_max_ms(void)
{
    return session_max_ms;
}

void game_set_cooldown_ms(uint32_t new_cooldown_ms)
{
    if (new_cooldown_ms > 60u * 60u * 1000u) {
        new_cooldown_ms = 60u * 60u * 1000u;
    }
    cooldown_ms = new_cooldown_ms;
}

uint32_t game_get_cooldown_ms(void)
{
    return cooldown_ms;
}

void game_set_motion_scale_percent(uint16_t scale_percent)
{
    if (scale_percent < 10u) {
        scale_percent = 10u;
    }
    if (scale_percent > 100u) {
        scale_percent = 100u;
    }
    motion_scale_percent = scale_percent;
}

uint16_t game_get_motion_scale_percent(void)
{
    return motion_scale_percent;
}

#if DEBUG_HARDWARE_ENABLED
bool game_laser_pulse(uint16_t duration_ms)
{
    if (duration_ms < LASER_PULSE_MIN_MS) {
        duration_ms = LASER_PULSE_MIN_MS;
    }
    if (duration_ms > LASER_PULSE_MAX_MS) {
        duration_ms = LASER_PULSE_MAX_MS;
    }

    game_set_enabled(false);
    hardware_servo_set_enabled(false);
    hardware_laser_set(true);
    laser_pulse_until_ms = now_ms() + duration_ms;
    game_state = GAME_STATE_DEBUG;
    PATTERN_LOG("manual laser pulse duration_ms=%u", duration_ms);
    return true;
}
#endif

bool game_laser_test_on(void)
{
    game_set_enabled(false);
    hardware_servo_set_enabled(false);
    hardware_laser_set(true);
    laser_pulse_until_ms = now_ms() + LASER_TEST_MAX_MS;
    game_state = GAME_STATE_DEBUG;
    PATTERN_LOG("manual laser test on max_ms=%u", (unsigned)LASER_TEST_MAX_MS);
    return true;
}

bool game_laser_test_on_inverted(void)
{
    game_set_enabled(false);
    hardware_servo_set_enabled(false);
    hardware_laser_set_raw_level(LASER_ACTIVE_LOW ? true : false);
    laser_pulse_until_ms = now_ms() + LASER_TEST_MAX_MS;
    game_state = GAME_STATE_DEBUG;
    PATTERN_LOG("manual laser inverted test on max_ms=%u", (unsigned)LASER_TEST_MAX_MS);
    return true;
}

void game_laser_test_off(void)
{
    game_set_enabled(false);
    hardware_laser_set(false);
    PATTERN_LOG("manual laser test off");
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
        if (session_has_expired()) {
            stop_session(true);
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

    uint16_t scale = motion_scale_percent;
    int32_t sx = ((int32_t)current_x * (int32_t)scale) / 100;
    int32_t sy = ((int32_t)current_y * (int32_t)scale) / 100;
    hardware_set_position_from_coord(clamp_coord(sx), clamp_coord(sy));
}

static int16_t choose_weighted_pattern_index(const pattern_pack_t *pack)
{
    uint16_t total = 0;

    for (uint16_t i = 0; i < pack->pattern_count; i++) {
        total += pack->patterns[i].weight;
    }

    if (total == 0) {
        return 0;
    }

    uint16_t draw = (uint16_t)random_range(0, total);

    for (uint16_t i = 0; i < pack->pattern_count; i++) {
        if (draw < pack->patterns[i].weight) {
            return (int16_t)i;
        }
        draw -= pack->patterns[i].weight;
    }

    return 0;
}

static int16_t find_capture_pattern_index(const pattern_pack_t *pack)
{
    if (pack == NULL) {
        return -1;
    }

    for (uint16_t i = 0; i < pack->pattern_count; i++) {
        if (strcmp(pack->patterns[i].id, "capture") == 0) {
            return (int16_t)i;
        }
    }

    if (pack->pattern_count > 0) {
        return (int16_t)(pack->pattern_count - 1);
    }
    return -1;
}

static bool step_has_position(const pattern_step_t *step)
{
    return step->type == STEP_HOLD ||
           step->type == STEP_MOVE ||
           step->type == STEP_JITTER ||
           step->type == STEP_OFF_MOVE;
}

static bool step_runtime_laser_on(const pattern_step_t *step)
{
    if (step->type == STEP_OFF_HOLD || step->type == STEP_OFF_MOVE) {
        return false;
    }
    return step->laser;
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

    hardware_laser_set(step_runtime_laser_on(first));

    for (uint32_t elapsed = 0; elapsed <= duration; elapsed += MOTION_TICK_MS) {
        if (!toy_enabled) {
            return;
        }
        if (session_has_expired()) {
            stop_session(true);
            return;
        }
        if (pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run interrupted during transition active_revision=%u current_revision=%u",
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision);
            return;
        }

        hardware_laser_set(step_runtime_laser_on(first));
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

    hardware_laser_set(step_runtime_laser_on(step));
    uint32_t duration = scaled_duration(step->duration_ms);
    uint32_t elapsed = 0;
    while (elapsed < duration) {
        if (!toy_enabled) {
            game_set_enabled(false);
            return;
        }
        if (session_has_expired()) {
            stop_session(true);
            return;
        }
        if (pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run interrupted during hold active_revision=%u current_revision=%u",
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision);
            return;
        }

        hardware_laser_set(step_runtime_laser_on(step));
        uint32_t delay = MOTION_TICK_MS;
        if (elapsed + delay > duration) {
            delay = duration - elapsed;
        }
        vTaskDelay(ms_to_ticks_min1(delay));
        elapsed += delay;
    }
}

static void run_move_step(const pattern_step_t *step)
{
    int16_t from_x = current_x;
    int16_t from_y = current_y;
    int16_t to_x = step->x;
    int16_t to_y = step->y;
    uint32_t duration = scaled_duration(step->duration_ms);

    hardware_laser_set(step_runtime_laser_on(step));

    for (uint32_t elapsed = 0; elapsed <= duration; elapsed += MOTION_TICK_MS) {
        if (!toy_enabled) {
            return;
        }
        if (session_has_expired()) {
            stop_session(true);
            return;
        }
        if (pattern_control_revision != active_run_revision) {
            PATTERN_LOG("run interrupted during move active_revision=%u current_revision=%u",
                        (unsigned)active_run_revision,
                        (unsigned)pattern_control_revision);
            return;
        }

        hardware_laser_set(step_runtime_laser_on(step));
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

    hardware_laser_set(step_runtime_laser_on(step));

    while (elapsed_total < duration) {
        if (!toy_enabled) {
            return;
        }
        if (session_has_expired()) {
            stop_session(true);
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
            if (session_has_expired()) {
                stop_session(true);
                return;
            }
            if (pattern_control_revision != active_run_revision) {
                PATTERN_LOG("run interrupted during jitter move active_revision=%u current_revision=%u",
                            (unsigned)active_run_revision,
                            (unsigned)pattern_control_revision);
                return;
            }

            hardware_laser_set(step_runtime_laser_on(step));
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
            if (game_state == GAME_STATE_DEBUG &&
                laser_pulse_until_ms != 0 &&
                !time_before(now_ms(), laser_pulse_until_ms)) {
                stop_laser_pulse();
                PATTERN_LOG("manual laser debug timeout");
            }
            if (game_state == GAME_STATE_COOLDOWN && !cooldown_is_active()) {
                cooldown_until_ms = 0;
                game_state = GAME_STATE_IDLE;
                PATTERN_LOG("cooldown finished");
            }
            vTaskDelay(ms_to_ticks_min1(200));
            continue;
        }
        if (session_has_expired()) {
            stop_session(true);
            continue;
        }

        const pattern_pack_t *pack = active_pack == NULL ? &default_pattern_pack : active_pack;
        uint8_t capture_every = pack->capture_every == 0 ? PATTERN_CAPTURE_EVERY : pack->capture_every;
        int16_t capture_index = find_capture_pattern_index(pack);
        int16_t pattern_index = -1;

        int16_t selected = selected_pattern_index;
        active_run_revision = pattern_control_revision;
        if (selected >= 0 && selected < (int16_t)pack->pattern_count) {
            pattern_index = selected;
            PATTERN_LOG("choose reason=manual index=%d id=%s speed=%u revision=%u",
                        selected,
                        pack->patterns[selected].id,
                        speed_percent,
                        (unsigned)active_run_revision);
        } else if (capture_index >= 0 && since_capture >= capture_every) {
            pattern_index = capture_index;
            since_capture = 0;
            PATTERN_LOG("choose reason=capture id=%s speed=%u revision=%u",
                        pack->patterns[pattern_index].id,
                        speed_percent,
                        (unsigned)active_run_revision);
        } else {
            pattern_index = choose_weighted_pattern_index(pack);
            since_capture++;
            PATTERN_LOG("choose reason=weighted id=%s since_capture=%u/%u speed=%u revision=%u",
                        pack->patterns[pattern_index].id,
                        since_capture,
                        capture_every,
                        speed_percent,
                        (unsigned)active_run_revision);
        }

        const pattern_t *pattern_to_run = &pack->patterns[pattern_index];
        if (pattern_store_is_active_pack(pack)) {
            char load_message[96];
            if (!pattern_store_load_pattern_by_index((uint16_t)pattern_index, &pattern_to_run, load_message, sizeof(load_message))) {
                PATTERN_LOG("pattern load failed index=%d msg=%s", pattern_index, load_message);
                wait_enabled_delay(scaled_duration((uint32_t)random_range(800, 1800)));
                continue;
            }
            PATTERN_LOG("pattern loaded on-demand index=%d id=%s", pattern_index, pattern_to_run->id);
        }

        run_pattern(pattern_to_run);

        hardware_laser_set(true);
        wait_enabled_delay(scaled_duration((uint32_t)random_range(800, 1800)));
    }
}
