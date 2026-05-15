#include "ota.h"

#include "app_util.h"
#include "game.h"
#include "logging.h"
#include "main.h"

#include <string.h>

#include "esp_common.h"
#include "freertos/task.h"
#include "spi_flash.h"
#include "upgrade.h"

#define OTA_USER1_FLASH_ADDR 0x1000u
#define OTA_USER2_FLASH_ADDR 0x101000u
#define OTA_IMAGE_MAGIC 0xEAu

#ifndef OTA_FLASH_ENABLED
#define OTA_FLASH_ENABLED 0
#endif

bool system_upgrade_userbin_set(uint8 userbin);

static volatile ota_state_t ota_state = OTA_STATE_IDLE;
static volatile uint32_t ota_expected_bytes = 0;
static volatile uint32_t ota_received_bytes = 0;
static uint32_t ota_target_addr = OTA_USER2_FLASH_ADDR;
static uint32_t ota_write_addr = OTA_USER2_FLASH_ADDR;
static uint8_t ota_partial[4];
static uint8_t ota_partial_len = 0;
static uint32_t ota_flash_buffer[256];
static char ota_status_text[96] = "OTA prete.";

static void ota_set_status(const char *status)
{
    strncpy(ota_status_text, status, sizeof(ota_status_text));
    ota_status_text[sizeof(ota_status_text) - 1] = '\0';
}

static void ota_reboot_task(void *arg)
{
    (void)arg;

    vTaskDelay(ms_to_ticks_min1(1500));
    OTA_LOG("rebooting to upgraded firmware");
    system_upgrade_reboot();
    vTaskDelete(NULL);
}

static uint8_t ota_target_userbin(void)
{
    return system_upgrade_userbin_check() == UPGRADE_FW_BIN1 ? UPGRADE_FW_BIN2 : UPGRADE_FW_BIN1;
}

static uint32_t ota_target_flash_addr(void)
{
    return ota_target_userbin() == UPGRADE_FW_BIN1 ? OTA_USER1_FLASH_ADDR : OTA_USER2_FLASH_ADDR;
}

static bool ota_erase_target(uint32_t content_length)
{
    uint16_t first_sector = (uint16_t)(ota_target_addr / SPI_FLASH_SEC_SIZE);
    uint16_t sector_count = (uint16_t)((content_length + SPI_FLASH_SEC_SIZE - 1) / SPI_FLASH_SEC_SIZE);

    for (uint16_t i = 0; i < sector_count; i++) {
        SpiFlashOpResult result = spi_flash_erase_sector((uint16)(first_sector + i));
        if (result != SPI_FLASH_RESULT_OK) {
            OTA_LOG("erase failed sector=%u result=%d", first_sector + i, result);
            return false;
        }
    }

    return true;
}

static bool ota_flash_write_aligned(const uint8_t *data, uint32_t len)
{
    while (len > 0) {
        uint32_t chunk = len > sizeof(ota_flash_buffer) ? sizeof(ota_flash_buffer) : len;
        chunk &= ~3u;
        if (chunk == 0) {
            return false;
        }

        memcpy(ota_flash_buffer, data, chunk);
        SpiFlashOpResult result = spi_flash_write((uint32)ota_write_addr, ota_flash_buffer, (uint32)chunk);
        if (result != SPI_FLASH_RESULT_OK) {
            OTA_LOG("write failed addr=0x%08x len=%u result=%d",
                    (unsigned)ota_write_addr, (unsigned)chunk, result);
            return false;
        }

        ota_write_addr += chunk;
        data += chunk;
        len -= chunk;
    }

    return true;
}

static bool ota_write_bytes(const uint8_t *data, uint32_t len)
{
    if (ota_partial_len > 0) {
        while (ota_partial_len < sizeof(ota_partial) && len > 0) {
            ota_partial[ota_partial_len++] = *data++;
            len--;
        }

        if (ota_partial_len == sizeof(ota_partial)) {
            if (!ota_flash_write_aligned(ota_partial, sizeof(ota_partial))) {
                return false;
            }
            ota_partial_len = 0;
        }
    }

    uint32_t aligned_len = len & ~3u;
    if (aligned_len > 0) {
        if (!ota_flash_write_aligned(data, aligned_len)) {
            return false;
        }
        data += aligned_len;
        len -= aligned_len;
    }

    while (len > 0) {
        ota_partial[ota_partial_len++] = *data++;
        len--;
    }

    return true;
}

static bool ota_flush_partial(void)
{
    if (ota_partial_len == 0) {
        return true;
    }

    while (ota_partial_len < sizeof(ota_partial)) {
        ota_partial[ota_partial_len++] = 0xFF;
    }

    bool ok = ota_flash_write_aligned(ota_partial, sizeof(ota_partial));
    ota_partial_len = 0;
    return ok;
}

void ota_init(void)
{
    uint8_t running = system_upgrade_userbin_check();
    ota_state = OTA_STATE_IDLE;
    ota_expected_bytes = 0;
    ota_received_bytes = 0;
    ota_set_status(OTA_FLASH_ENABLED ? "OTA prete." : "OTA desactivee sur cette build.");
    OTA_LOG("init running=%s expected_upload=%s flag=%d",
            ota_get_running_bin_name(),
            ota_get_expected_upload_bin_name(),
            system_upgrade_flag_check());

    if (running != UPGRADE_FW_BIN1 && running != UPGRADE_FW_BIN2) {
        OTA_LOG("unknown running user bin=%d", running);
    }
}

bool ota_begin_update(uint32_t content_length)
{
    if (!OTA_FLASH_ENABLED) {
        ota_state = OTA_STATE_FAILED;
        ota_set_status("Compiler et flasher l'environnement d1_mini_pro_ota.");
        OTA_LOG("begin rejected ota flash disabled");
        return false;
    }

    if (ota_state == OTA_STATE_RECEIVING || ota_state == OTA_STATE_REBOOT_PENDING) {
        ota_set_status("OTA deja en cours.");
        OTA_LOG("begin rejected state=%d", ota_state);
        return false;
    }

    if (content_length == 0 || content_length > OTA_MAX_FIRMWARE_SIZE) {
        ota_state = OTA_STATE_FAILED;
        ota_set_status("Taille firmware invalide.");
        OTA_LOG("begin rejected content_length=%u max=%u",
                (unsigned)content_length,
                (unsigned)OTA_MAX_FIRMWARE_SIZE);
        return false;
    }

    game_set_enabled(false);
    ota_expected_bytes = content_length;
    ota_received_bytes = 0;
    ota_target_addr = ota_target_flash_addr();
    ota_write_addr = ota_target_addr;
    ota_partial_len = 0;
    memset(ota_partial, 0xFF, sizeof(ota_partial));
    ota_state = OTA_STATE_RECEIVING;
    ota_set_status("Reception OTA en cours.");

    system_upgrade_flag_set(UPGRADE_FLAG_START);
    if (!ota_erase_target(content_length)) {
        ota_fail_update("Effacement flash OTA echoue.");
        return false;
    }

    OTA_LOG("begin length=%u running=%s target=%s",
            (unsigned)content_length,
            ota_get_running_bin_name(),
            ota_get_expected_upload_bin_name());
    return true;
}

bool ota_write_chunk(const uint8_t *data, uint32_t len)
{
    if (ota_state != OTA_STATE_RECEIVING || data == NULL || len == 0) {
        OTA_LOG("write rejected state=%d data=%p len=%u",
                ota_state, data, (unsigned)len);
        return false;
    }

    if (ota_received_bytes + len > ota_expected_bytes) {
        ota_fail_update("Firmware plus grand que Content-Length.");
        return false;
    }

    if (ota_received_bytes == 0 && data[0] != OTA_IMAGE_MAGIC) {
        ota_fail_update("Image OTA invalide.");
        return false;
    }

    if (!ota_write_bytes(data, len)) {
        ota_fail_update("Ecriture flash OTA echouee.");
        return false;
    }

    ota_received_bytes += len;
    return true;
}

bool ota_finish_update(void)
{
    if (ota_state != OTA_STATE_RECEIVING) {
        OTA_LOG("finish rejected state=%d", ota_state);
        return false;
    }

    if (ota_received_bytes != ota_expected_bytes) {
        ota_fail_update("Firmware incomplet.");
        return false;
    }

    if (!ota_flush_partial()) {
        ota_fail_update("Finalisation flash OTA echouee.");
        return false;
    }

    if (!system_upgrade_userbin_set(ota_target_userbin())) {
        ota_fail_update("Selection du firmware OTA echouee.");
        return false;
    }

    system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
    ota_state = OTA_STATE_REBOOT_PENDING;
    ota_set_status("OTA recue. Redemarrage.");
    OTA_LOG("finish received=%u flag=%d",
            (unsigned)ota_received_bytes,
            system_upgrade_flag_check());

    xTaskCreate(ota_reboot_task, "ota_reboot", 512, NULL, 6, NULL);
    return true;
}

void ota_fail_update(const char *reason)
{
    system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
    ota_state = OTA_STATE_FAILED;
    ota_set_status(reason == NULL ? "OTA echouee." : reason);
    OTA_LOG("failed reason=%s received=%u expected=%u",
            reason == NULL ? "unknown" : reason,
            (unsigned)ota_received_bytes,
            (unsigned)ota_expected_bytes);
}

bool ota_is_running(void)
{
    return ota_state == OTA_STATE_RECEIVING || ota_state == OTA_STATE_REBOOT_PENDING;
}

ota_state_t ota_get_state(void)
{
    return ota_state;
}

uint32_t ota_get_expected_bytes(void)
{
    return ota_expected_bytes;
}

uint32_t ota_get_received_bytes(void)
{
    return ota_received_bytes;
}

const char *ota_get_status_text(void)
{
    return ota_status_text;
}

const char *ota_get_running_bin_name(void)
{
    return system_upgrade_userbin_check() == UPGRADE_FW_BIN1 ? "user1.bin" : "user2.bin";
}

const char *ota_get_expected_upload_bin_name(void)
{
    return "firmware.ota.bin";
}
