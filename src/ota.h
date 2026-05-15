#ifndef LASER_CAT_TOY_OTA_H
#define LASER_CAT_TOY_OTA_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_RECEIVING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
    OTA_STATE_REBOOT_PENDING
} ota_state_t;

void ota_init(void);
bool ota_begin_update(uint32_t content_length);
bool ota_write_chunk(const uint8_t *data, uint32_t len);
bool ota_finish_update(void);
void ota_fail_update(const char *reason);
bool ota_is_running(void);
ota_state_t ota_get_state(void);
uint32_t ota_get_expected_bytes(void);
uint32_t ota_get_received_bytes(void);
const char *ota_get_status_text(void);
const char *ota_get_running_bin_name(void);
const char *ota_get_expected_upload_bin_name(void);

#endif
