#include "pattern_store.h"

#include "logging.h"
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
    free(buf);
    return ok;
}
