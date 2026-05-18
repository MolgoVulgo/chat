#include "pattern_dat.h"

#include "logging.h"
#include "main.h"

#include <string.h>
#include <stdio.h>

#define DAT_MAGIC 0x4E54504Cu
#define DAT_VERSION 2u
#define DAT_ENDIAN_LE 1u
#define DAT_HEADER_SIZE 48u
#define DAT_INDEX_SIZE 48u
#define DAT_POINT_SIZE 10u
#define DAT_MAX_FILE_SIZE 65536u

static pattern_t dat_patterns[PATTERN_MAX_PATTERNS];
static char dat_ids[PATTERN_MAX_PATTERNS][PATTERN_MAX_ID_LEN];
static pattern_pack_t dat_pack;

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

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint8_t i = 0; i < 8; i++) {
        uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
        crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
    return crc;
}

static uint32_t crc32_buf(const uint8_t *buf, uint32_t len, uint32_t zero_start, uint32_t zero_end)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = (i >= zero_start && i < zero_end) ? 0u : buf[i];
        crc = crc32_update(crc, b);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool pattern_dat_load(const uint8_t *data,
                      uint32_t len,
                      const pattern_pack_t **pack,
                      char *message,
                      size_t message_len)
{
    if (pack != NULL) {
        *pack = NULL;
    }
    if (message != NULL && message_len > 0) {
        message[0] = '\0';
    }
    if (data == NULL || len < DAT_HEADER_SIZE || len > DAT_MAX_FILE_SIZE) {
        if (message != NULL && message_len > 0) {
            snprintf(message, message_len, "patterns.dat invalide (taille).");
        }
        return false;
    }

    uint32_t magic = read_le32(data + 0);
    uint8_t version = data[4];
    uint8_t endian = data[5];
    uint16_t header_size = read_le16(data + 6);
    uint32_t file_size = read_le32(data + 8);
    uint32_t header_crc32 = read_le32(data + 12);
    uint32_t payload_crc32 = read_le32(data + 16);
    uint32_t index_offset = read_le32(data + 20);
    uint16_t pattern_count = read_le16(data + 24);
    uint16_t index_size = read_le16(data + 26);
    uint32_t data_offset = read_le32(data + 28);
    uint16_t point_size = read_le16(data + 32);

    if (magic != DAT_MAGIC || version != DAT_VERSION || endian != DAT_ENDIAN_LE ||
        header_size != DAT_HEADER_SIZE || file_size != len ||
        index_offset != DAT_HEADER_SIZE || index_size != DAT_INDEX_SIZE ||
        point_size != DAT_POINT_SIZE) {
        if (message != NULL && message_len > 0) {
            snprintf(message, message_len, "header patterns.dat invalide.");
        }
        return false;
    }
    if (pattern_count == 0 || pattern_count > PATTERN_MAX_PATTERNS) {
        if (message != NULL && message_len > 0) {
            snprintf(message, message_len, "nombre de patterns invalide.");
        }
        return false;
    }
    if (data_offset != (index_offset + ((uint32_t)pattern_count * DAT_INDEX_SIZE))) {
        if (message != NULL && message_len > 0) {
            snprintf(message, message_len, "offset data invalide.");
        }
        return false;
    }

    uint32_t calc_header_crc = crc32_buf(data, DAT_HEADER_SIZE, 12u, 16u);
    uint32_t calc_payload_crc = crc32_buf(data + index_offset, len - index_offset, UINT32_MAX, UINT32_MAX);
    if (calc_header_crc != header_crc32 || calc_payload_crc != payload_crc32) {
        if (message != NULL && message_len > 0) {
            snprintf(message, message_len, "CRC patterns.dat invalide.");
        }
        return false;
    }

    memset(dat_patterns, 0, sizeof(dat_patterns));
    memset(dat_ids, 0, sizeof(dat_ids));

    uint32_t total_points = 0;
    for (uint16_t i = 0; i < pattern_count; i++) {
        const uint8_t *rec = data + index_offset + ((uint32_t)i * DAT_INDEX_SIZE);
        uint32_t point_offset = read_le32(rec + 24);
        uint16_t point_count = read_le16(rec + 36);
        uint8_t weight = rec[38];
        uint32_t points_end = point_offset + ((uint32_t)point_count * DAT_POINT_SIZE);

        if (point_count == 0 || points_end > len || point_offset < data_offset) {
            if (message != NULL && message_len > 0) {
                snprintf(message, message_len, "index pattern invalide.");
            }
            return false;
        }
        if (total_points + (uint32_t)point_count > PATTERN_MAX_TOTAL_STEPS) {
            if (message != NULL && message_len > 0) {
                snprintf(message, message_len, "trop de points runtime.");
            }
            return false;
        }

        memcpy(dat_ids[i], rec, 24);
        dat_ids[i][PATTERN_MAX_ID_LEN - 1] = '\0';
        if (dat_ids[i][0] == '\0') {
            snprintf(dat_ids[i], sizeof(dat_ids[i]), "pattern_%u", (unsigned)i);
        }

        dat_patterns[i].id = dat_ids[i];
        dat_patterns[i].name = dat_ids[i];
        dat_patterns[i].weight = weight;
        dat_patterns[i].step_count = point_count;
        dat_patterns[i].steps = NULL;
        total_points += point_count;
    }

    dat_pack.patterns = dat_patterns;
    dat_pack.pattern_count = pattern_count;
    dat_pack.capture_every = PATTERN_CAPTURE_EVERY;
    dat_pack.source_name = "uploaded patterns.dat";
    dat_pack.json_source = NULL;
    dat_pack.json_source_len = 0;
    if (pack != NULL) {
        *pack = &dat_pack;
    }
    if (message != NULL && message_len > 0) {
        snprintf(message, message_len, "patterns.dat charge (%u patterns, %u points).",
                 (unsigned)pattern_count, (unsigned)total_points);
    }
    return true;
}
