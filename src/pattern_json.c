#include "pattern_json.h"

#include "main.h"

#include <stdio.h>
#include <string.h>

#if PATTERN_JSON_UPLOAD_ENABLED
#include "json/cJSON.h"

static pattern_step_t uploaded_steps[PATTERN_MAX_TOTAL_STEPS];
static pattern_t uploaded_patterns[PATTERN_MAX_PATTERNS];
static char uploaded_ids[PATTERN_MAX_PATTERNS][PATTERN_MAX_ID_LEN];
static char uploaded_names[PATTERN_MAX_PATTERNS][PATTERN_MAX_NAME_LEN];
static pattern_pack_t uploaded_pack;
#endif

static void set_message(char *message, size_t message_len, const char *text)
{
    if (message_len == 0) {
        return;
    }

    strncpy(message, text, message_len);
    message[message_len - 1] = '\0';
}

#if PATTERN_JSON_UPLOAD_ENABLED
static int json_type(const cJSON *item)
{
    return item == NULL ? 0 : (item->type & 0xFF);
}

static cJSON *object_item(cJSON *object, const char *name, int expected_type)
{
    cJSON *item = cJSON_GetObjectItem(object, name);
    if (json_type(item) != expected_type) {
        return NULL;
    }
    return item;
}

static bool json_bool(cJSON *object, const char *name, bool default_value)
{
    cJSON *item = cJSON_GetObjectItem(object, name);
    if (json_type(item) == cJSON_True) {
        return true;
    }
    if (json_type(item) == cJSON_False) {
        return false;
    }
    return default_value;
}

static bool step_type_from_string(const char *type, pattern_step_type_t *out)
{
    if (strcmp(type, "hold") == 0) {
        *out = STEP_HOLD;
    } else if (strcmp(type, "move") == 0) {
        *out = STEP_MOVE;
    } else if (strcmp(type, "jitter") == 0) {
        *out = STEP_JITTER;
    } else if (strcmp(type, "off_hold") == 0) {
        *out = STEP_OFF_HOLD;
    } else if (strcmp(type, "off_move") == 0) {
        *out = STEP_OFF_MOVE;
    } else {
        return false;
    }

    return true;
}

static bool step_has_position(pattern_step_type_t type)
{
    return type == STEP_HOLD ||
           type == STEP_MOVE ||
           type == STEP_JITTER ||
           type == STEP_OFF_MOVE;
}

static bool parse_coord(cJSON *step, const char *name, int16_t *out)
{
    cJSON *item = object_item(step, name, cJSON_Number);
    if (item == NULL || item->valuedouble < -1.0 || item->valuedouble > 1.0) {
        return false;
    }

    double scaled = item->valuedouble * 1000.0;
    *out = (int16_t)(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
    return true;
}

static bool id_seen(const char *id, uint8_t pattern_count)
{
    for (uint8_t i = 0; i < pattern_count; i++) {
        if (strcmp(uploaded_ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static bool parse_step(cJSON *step, pattern_step_t *out, char *message, size_t message_len)
{
    cJSON *type_item = object_item(step, "type", cJSON_String);
    cJSON *duration_item = object_item(step, "duration_ms", cJSON_Number);
    pattern_step_type_t type;

    if (type_item == NULL || !step_type_from_string(type_item->valuestring, &type)) {
        set_message(message, message_len, "Type de step invalide.");
        return false;
    }
    if (duration_item == NULL ||
        duration_item->valueint < PATTERN_MIN_DURATION_MS ||
        duration_item->valueint > UINT16_MAX) {
        set_message(message, message_len, "Duree de step invalide.");
        return false;
    }
    if ((type == STEP_HOLD || type == STEP_MOVE || type == STEP_JITTER) &&
        duration_item->valueint < PATTERN_MIN_VISIBLE_MS) {
        set_message(message, message_len, "Step visible trop courte.");
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->type = type;
    out->duration_ms = (uint16_t)duration_item->valueint;
    out->laser = json_bool(step, "laser", type != STEP_OFF_HOLD && type != STEP_OFF_MOVE);

    if (type == STEP_OFF_HOLD || type == STEP_OFF_MOVE) {
        out->laser = false;
    }

    if (step_has_position(type)) {
        if (!parse_coord(step, "x", &out->x) || !parse_coord(step, "y", &out->y)) {
            set_message(message, message_len, "Coordonnees x/y invalides.");
            return false;
        }
    }

    if (type == STEP_JITTER) {
        cJSON *amplitude = object_item(step, "amplitude", cJSON_Number);
        if (amplitude == NULL || amplitude->valuedouble < 0.0 || amplitude->valuedouble > 1.0) {
            set_message(message, message_len, "Amplitude jitter invalide.");
            return false;
        }
        int scaled = (int)(amplitude->valuedouble * 1000.0 + 0.5);
        if (scaled > PATTERN_MAX_JITTER_AMPLITUDE) {
            set_message(message, message_len, "Amplitude jitter trop grande.");
            return false;
        }
        out->amplitude = (uint16_t)scaled;
    }

    return true;
}

bool pattern_json_load(const char *json,
                       uint32_t json_len,
                       const pattern_pack_t **pack,
                       char *message,
                       size_t message_len)
{
    if (pack == NULL) {
        return false;
    }
    *pack = NULL;

    if (json == NULL || json_len == 0 || json_len > PATTERN_JSON_UPLOAD_MAX_BYTES) {
        set_message(message, message_len, "Taille JSON invalide.");
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL || json_type(root) != cJSON_Object) {
        cJSON_Delete(root);
        set_message(message, message_len, "JSON invalide.");
        return false;
    }

    cJSON *schema = object_item(root, "schema", cJSON_String);
    if (schema == NULL || strcmp(schema->valuestring, PATTERN_SCHEMA) != 0) {
        cJSON_Delete(root);
        set_message(message, message_len, "Schema JSON non supporte.");
        return false;
    }

    cJSON *patterns = object_item(root, "patterns", cJSON_Array);
    int pattern_count = cJSON_GetArraySize(patterns);
    if (patterns == NULL || pattern_count <= 0 || pattern_count > PATTERN_MAX_PATTERNS) {
        cJSON_Delete(root);
        set_message(message, message_len, "Nombre de patterns invalide.");
        return false;
    }

    memset(uploaded_steps, 0, sizeof(uploaded_steps));
    memset(uploaded_patterns, 0, sizeof(uploaded_patterns));
    memset(uploaded_ids, 0, sizeof(uploaded_ids));
    memset(uploaded_names, 0, sizeof(uploaded_names));

    uint16_t step_offset = 0;
    for (int i = 0; i < pattern_count; i++) {
        cJSON *pattern = cJSON_GetArrayItem(patterns, i);
        cJSON *id = object_item(pattern, "id", cJSON_String);
        cJSON *name = object_item(pattern, "name", cJSON_String);
        cJSON *steps = object_item(pattern, "steps", cJSON_Array);
        cJSON *weight = object_item(pattern, "weight", cJSON_Number);
        int step_count = cJSON_GetArraySize(steps);

        if (json_type(pattern) != cJSON_Object || id == NULL || id->valuestring[0] == '\0' ||
            steps == NULL || step_count <= 0) {
            cJSON_Delete(root);
            set_message(message, message_len, "Pattern invalide.");
            return false;
        }
        if (id_seen(id->valuestring, (uint8_t)i)) {
            cJSON_Delete(root);
            set_message(message, message_len, "Id de pattern duplique.");
            return false;
        }
        if (step_offset + step_count > PATTERN_MAX_TOTAL_STEPS) {
            cJSON_Delete(root);
            set_message(message, message_len, "Trop de steps dans le pack.");
            return false;
        }

        strncpy(uploaded_ids[i], id->valuestring, sizeof(uploaded_ids[i]));
        uploaded_ids[i][sizeof(uploaded_ids[i]) - 1] = '\0';
        if (name != NULL && name->valuestring[0] != '\0') {
            strncpy(uploaded_names[i], name->valuestring, sizeof(uploaded_names[i]));
        } else {
            strncpy(uploaded_names[i], uploaded_ids[i], sizeof(uploaded_names[i]));
        }
        uploaded_names[i][sizeof(uploaded_names[i]) - 1] = '\0';

        uploaded_patterns[i].id = uploaded_ids[i];
        uploaded_patterns[i].name = uploaded_names[i];
        uploaded_patterns[i].weight = weight == NULL || weight->valueint < 0 ? 1 : (uint8_t)weight->valueint;
        uploaded_patterns[i].step_count = (uint16_t)step_count;
        uploaded_patterns[i].steps = &uploaded_steps[step_offset];

        for (int j = 0; j < step_count; j++) {
            cJSON *step = cJSON_GetArrayItem(steps, j);
            if (json_type(step) != cJSON_Object ||
                !parse_step(step, &uploaded_steps[step_offset + j], message, message_len)) {
                cJSON_Delete(root);
                return false;
            }
        }

        step_offset += (uint16_t)step_count;
    }

    uint8_t capture_every = PATTERN_CAPTURE_EVERY;
    cJSON *runtime = object_item(root, "runtime", cJSON_Object);
    if (runtime != NULL) {
        cJSON *capture = object_item(runtime, "capture_every", cJSON_Number);
        if (capture != NULL && capture->valueint >= 0 && capture->valueint <= UINT8_MAX) {
            capture_every = (uint8_t)capture->valueint;
        }
    }

    uploaded_pack.patterns = uploaded_patterns;
    uploaded_pack.pattern_count = (uint8_t)pattern_count;
    uploaded_pack.capture_every = capture_every;
    uploaded_pack.source_name = "uploaded JSON";
    uploaded_pack.json_source = NULL;
    uploaded_pack.json_source_len = json_len;

    cJSON_Delete(root);
    *pack = &uploaded_pack;
    set_message(message, message_len, "JSON valide et actif.");
    return true;
}
#else
bool pattern_json_load(const char *json,
                       uint32_t json_len,
                       const pattern_pack_t **pack,
                       char *message,
                       size_t message_len)
{
    (void)json;
    (void)json_len;
    if (pack != NULL) {
        *pack = NULL;
    }
    set_message(message, message_len,
                "Upload JSON desactive: regenerer src/default_patterns.c avec l'outil Python.");
    return false;
}
#endif
