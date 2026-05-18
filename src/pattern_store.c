#include "pattern_store.h"

#include "logging.h"
#include "main.h"
#include "pattern_dat.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_common.h"
#include "spiffs/spiffs.h"

#ifndef PATTERN_SPIFFS_ADDR
#define PATTERN_SPIFFS_ADDR  (14u * 1024u * 1024u)
#endif
#ifndef PATTERN_SPIFFS_SIZE
#define PATTERN_SPIFFS_SIZE  (2u * 1024u * 1024u)
#endif
#define PATTERN_STORE_FILE   "patterns.dat"
#define PATTERN_STORE_TEMP   "patterns.tmp"
#define PATTERN_STORE_BACKUP "patterns.bak"
#define PATTERN_STORE_MAX_UPLOAD_BYTES 65536u
#define DAT_HEADER_SIZE 48u
#define DAT_INDEX_SIZE 48u
#define DAT_POINT_SIZE 10u

static bool store_ready = false;
static char store_status[96] = "SPIFFS non initialise.";
static spiffs store_fs;
static uint8_t work_buf[256];
static uint8_t fd_buf[192];
static uint8_t cache_buf[(128u + 32u) * 4u];
static uint32_t rw_buf[(128u + 8u) / 4u];
static spiffs_file upload_fd = -1;
static uint32_t upload_expected = 0;
static uint32_t upload_written = 0;
static uint32_t pattern_point_offsets[PATTERN_MAX_PATTERNS];
static uint16_t pattern_point_counts[PATTERN_MAX_PATTERNS];
static pattern_step_t *runtime_steps = NULL;
static uint16_t runtime_steps_capacity = 0;
static pattern_t runtime_pattern;
static const pattern_pack_t *active_store_pack = NULL;

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t x_u16_to_internal(uint16_t x)
{
    return (int16_t)((int32_t)x * 2 - 1000);
}

static int16_t y_u16_to_internal(uint16_t y)
{
    return (int16_t)(1000 - ((int32_t)y * 2));
}

static s32_t flash_rw(uint32_t addr, uint32_t size, uint8_t *data, bool write)
{
    uint32_t aligned_addr = addr & ~3u;
    uint32_t offset = addr - aligned_addr;
    uint32_t aligned_size = (offset + size + 3u) & ~3u;
    if (aligned_size > sizeof(rw_buf)) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    if (spi_flash_read(aligned_addr, rw_buf, aligned_size) != 0) {
        return SPIFFS_ERR_INTERNAL;
    }
    if (!write) {
        memcpy(data, ((uint8_t *)rw_buf) + offset, size);
        return SPIFFS_OK;
    }
    memcpy(((uint8_t *)rw_buf) + offset, data, size);
    return spi_flash_write(aligned_addr, rw_buf, aligned_size) == 0 ? SPIFFS_OK : SPIFFS_ERR_INTERNAL;
}

static s32_t hal_read(uint32_t addr, uint32_t size, uint8_t *dst) { return flash_rw(addr, size, dst, false); }
static s32_t hal_write(uint32_t addr, uint32_t size, uint8_t *src) { return flash_rw(addr, size, src, true); }
static s32_t hal_erase(uint32_t addr, uint32_t size)
{
    if (size != 4096u || (addr % 4096u) != 0) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    return spi_flash_erase_sector(addr / 4096u) == 0 ? SPIFFS_OK : SPIFFS_ERR_INTERNAL;
}

void pattern_store_init(void)
{
    spiffs_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.phys_addr = PATTERN_SPIFFS_ADDR;
    cfg.phys_size = PATTERN_SPIFFS_SIZE;
    cfg.phys_erase_block = 4096u;
    cfg.log_block_size = 4096u;
    cfg.log_page_size = 128u;
    cfg.hal_read_f = hal_read;
    cfg.hal_write_f = hal_write;
    cfg.hal_erase_f = hal_erase;

    if (SPIFFS_mount(&store_fs, &cfg, work_buf, fd_buf, sizeof(fd_buf), cache_buf, sizeof(cache_buf), 0) != SPIFFS_OK) {
        SPIFFS_format(&store_fs);
        if (SPIFFS_mount(&store_fs, &cfg, work_buf, fd_buf, sizeof(fd_buf), cache_buf, sizeof(cache_buf), 0) != SPIFFS_OK) {
            store_ready = false;
            snprintf(store_status, sizeof(store_status), "SPIFFS indisponible.");
            return;
        }
    }
    store_ready = true;
    snprintf(store_status, sizeof(store_status), "SPIFFS OK.");
}

const char *pattern_store_get_status(void)
{
    return store_status;
}

bool pattern_store_has_file(void)
{
    spiffs_stat st;
    if (!store_ready) {
        return false;
    }
    return SPIFFS_stat(&store_fs, PATTERN_STORE_FILE, &st) == SPIFFS_OK;
}

bool pattern_store_begin_upload(uint32_t expected_len, char *message, size_t message_len)
{
    if (!store_ready) {
        snprintf(message, message_len, "SPIFFS indisponible.");
        return false;
    }
    if (expected_len == 0 || expected_len > PATTERN_STORE_MAX_UPLOAD_BYTES) {
        snprintf(message, message_len, "Taille patterns.dat invalide.");
        return false;
    }
    if (upload_fd >= 0) {
        SPIFFS_close(&store_fs, upload_fd);
        upload_fd = -1;
    }
    SPIFFS_remove(&store_fs, PATTERN_STORE_TEMP);
    upload_fd = SPIFFS_open(&store_fs, PATTERN_STORE_TEMP, SPIFFS_CREAT | SPIFFS_TRUNC | SPIFFS_WRONLY, 0);
    if (upload_fd < 0) {
        snprintf(message, message_len, "Ouverture temp echouee.");
        return false;
    }
    upload_expected = expected_len;
    upload_written = 0;
    return true;
}

bool pattern_store_write_upload_chunk(const uint8_t *data, uint32_t len, char *message, size_t message_len)
{
    if (upload_fd < 0 || data == NULL || len == 0) {
        snprintf(message, message_len, "Chunk invalide.");
        return false;
    }
    if (upload_written + len > upload_expected) {
        snprintf(message, message_len, "Upload depasse la taille.");
        return false;
    }
    if (SPIFFS_write(&store_fs, upload_fd, (void *)data, len) != (s32_t)len) {
        snprintf(message, message_len, "Ecriture SPIFFS echouee.");
        return false;
    }
    upload_written += len;
    return true;
}

bool pattern_store_finish_upload(char *message, size_t message_len)
{
    if (upload_fd < 0) {
        snprintf(message, message_len, "Upload non initialise.");
        return false;
    }
    SPIFFS_close(&store_fs, upload_fd);
    upload_fd = -1;
    if (upload_written != upload_expected) {
        SPIFFS_remove(&store_fs, PATTERN_STORE_TEMP);
        snprintf(message, message_len, "Upload incomplet.");
        return false;
    }

    SPIFFS_remove(&store_fs, PATTERN_STORE_BACKUP);
    SPIFFS_rename(&store_fs, PATTERN_STORE_FILE, PATTERN_STORE_BACKUP);
    SPIFFS_remove(&store_fs, PATTERN_STORE_FILE);
    if (SPIFFS_rename(&store_fs, PATTERN_STORE_TEMP, PATTERN_STORE_FILE) != SPIFFS_OK) {
        SPIFFS_rename(&store_fs, PATTERN_STORE_BACKUP, PATTERN_STORE_FILE);
        snprintf(message, message_len, "Activation fichier echouee.");
        return false;
    }
    snprintf(message, message_len, "patterns.dat enregistre (%u octets).", (unsigned)upload_written);
    return true;
}

bool pattern_store_load_active_pack(const pattern_pack_t **pack, char *message, size_t message_len)
{
    spiffs_stat st;
    spiffs_file fd;
    uint8_t *buf;
    bool ok;

    if (pack != NULL) {
        *pack = NULL;
    }
    active_store_pack = NULL;
    if (!store_ready) {
        snprintf(message, message_len, "SPIFFS indisponible.");
        return false;
    }
    if (SPIFFS_stat(&store_fs, PATTERN_STORE_FILE, &st) != SPIFFS_OK || st.size == 0 || st.size > PATTERN_STORE_MAX_UPLOAD_BYTES) {
        snprintf(message, message_len, "patterns.dat absent/invalide.");
        return false;
    }

    fd = SPIFFS_open(&store_fs, PATTERN_STORE_FILE, SPIFFS_RDONLY, 0);
    if (fd < 0) {
        snprintf(message, message_len, "Ouverture patterns.dat echouee.");
        return false;
    }
    buf = (uint8_t *)malloc(st.size);
    if (buf == NULL) {
        SPIFFS_close(&store_fs, fd);
        snprintf(message, message_len, "Memoire insuffisante.");
        return false;
    }
    if (SPIFFS_read(&store_fs, fd, buf, st.size) != (s32_t)st.size) {
        SPIFFS_close(&store_fs, fd);
        free(buf);
        snprintf(message, message_len, "Lecture patterns.dat echouee.");
        return false;
    }
    SPIFFS_close(&store_fs, fd);
    ok = pattern_dat_load(buf, st.size, pack, message, message_len);
    if (ok && pack != NULL && *pack != NULL) {
        uint32_t index_offset = read_le32(buf + 20);
        uint16_t pattern_count = read_le16(buf + 24);
        uint32_t data_offset = read_le32(buf + 28);
        for (uint16_t i = 0; i < pattern_count && i < PATTERN_MAX_PATTERNS; i++) {
            const uint8_t *rec = buf + index_offset + ((uint32_t)i * DAT_INDEX_SIZE);
            uint32_t point_offset = read_le32(rec + 24);
            uint16_t point_count = read_le16(rec + 36);
            uint32_t points_end = point_offset + ((uint32_t)point_count * DAT_POINT_SIZE);
            if (point_count == 0 || point_offset < data_offset || points_end > st.size) {
                ok = false;
                snprintf(message, message_len, "index patterns.dat invalide.");
                break;
            }
            pattern_point_offsets[i] = point_offset;
            pattern_point_counts[i] = point_count;
        }
        if (ok) {
            active_store_pack = *pack;
        }
    }
    free(buf);
    return ok;
}

bool pattern_store_load_pattern_by_index(uint16_t pattern_index,
                                         const pattern_t **pattern,
                                         char *message,
                                         size_t message_len)
{
    if (pattern != NULL) {
        *pattern = NULL;
    }
    if (!store_ready || active_store_pack == NULL) {
        snprintf(message, message_len, "Pack store indisponible.");
        return false;
    }
    if (pattern_index >= active_store_pack->pattern_count) {
        snprintf(message, message_len, "Index pattern invalide.");
        return false;
    }

    uint16_t point_count = pattern_point_counts[pattern_index];
    uint32_t point_offset = pattern_point_offsets[pattern_index];
    if (point_count == 0 || point_count > PATTERN_MAX_TOTAL_STEPS) {
        snprintf(message, message_len, "Nombre de points invalide.");
        return false;
    }
    if (runtime_steps_capacity < point_count) {
        pattern_step_t *new_steps = (pattern_step_t *)realloc(runtime_steps, point_count * sizeof(pattern_step_t));
        if (new_steps == NULL) {
            snprintf(message, message_len, "Memoire insuffisante steps.");
            return false;
        }
        runtime_steps = new_steps;
        runtime_steps_capacity = point_count;
    }

    spiffs_file fd = SPIFFS_open(&store_fs, PATTERN_STORE_FILE, SPIFFS_RDONLY, 0);
    if (fd < 0) {
        snprintf(message, message_len, "Ouverture patterns.dat echouee.");
        return false;
    }
    if (SPIFFS_lseek(&store_fs, fd, (s32_t)point_offset, SPIFFS_SEEK_SET) < 0) {
        SPIFFS_close(&store_fs, fd);
        snprintf(message, message_len, "Seek patterns.dat echoue.");
        return false;
    }

    uint8_t pt[DAT_POINT_SIZE];
    for (uint16_t i = 0; i < point_count; i++) {
        if (SPIFFS_read(&store_fs, fd, pt, DAT_POINT_SIZE) != DAT_POINT_SIZE) {
            SPIFFS_close(&store_fs, fd);
            snprintf(message, message_len, "Lecture point patterns.dat echouee.");
            return false;
        }
        uint16_t x = read_le16(pt + 0);
        uint16_t y = read_le16(pt + 2);
        uint16_t duration = read_le16(pt + 4);
        uint8_t laser = pt[6];
        uint8_t action = pt[7];
        uint16_t arg = read_le16(pt + 8);
        if (x > 1000 || y > 1000 || duration < PATTERN_MIN_DURATION_MS || duration > 10000 ||
            laser > 1 || action > 4) {
            SPIFFS_close(&store_fs, fd);
            snprintf(message, message_len, "Point pattern invalide.");
            return false;
        }
        pattern_step_t *dst = &runtime_steps[i];
        dst->type = action == 0 ? STEP_HOLD :
                    action == 1 ? STEP_MOVE :
                    action == 2 ? STEP_JITTER :
                    action == 3 ? STEP_OFF_HOLD : STEP_OFF_MOVE;
        dst->laser = laser != 0;
        dst->x = x_u16_to_internal(x);
        dst->y = y_u16_to_internal(y);
        dst->duration_ms = duration;
        dst->amplitude = arg > PATTERN_MAX_JITTER_AMPLITUDE ? PATTERN_MAX_JITTER_AMPLITUDE : arg;
    }
    SPIFFS_close(&store_fs, fd);

    runtime_pattern = active_store_pack->patterns[pattern_index];
    runtime_pattern.steps = runtime_steps;
    if (pattern != NULL) {
        *pattern = &runtime_pattern;
    }
    snprintf(message, message_len, "Pattern charge: %s (%u points).",
             runtime_pattern.id, (unsigned)point_count);
    return true;
}

bool pattern_store_is_active_pack(const pattern_pack_t *pack)
{
    return pack != NULL && pack == active_store_pack;
}
