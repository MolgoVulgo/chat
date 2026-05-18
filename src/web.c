#include "web.h"

#include "app_util.h"
#include "game.h"
#include "hardware.h"
#include "logging.h"
#include "main.h"
#include "pattern_store.h"
#include "web_assets.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#include "esp_common.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

typedef struct {
    char ssid[33];
    sint8 rssi;
    AUTH_MODE authmode;
    uint8 channel;
} wifi_scan_result_t;

static wifi_scan_result_t wifi_scan_results[WIFI_SCAN_MAX_RESULTS];
static volatile uint8_t wifi_scan_count = 0;
static volatile bool wifi_scan_running = false;
static volatile bool wifi_scan_requested = false;
static volatile bool config_ap_active = true;
static char wifi_status_message[96] = "Aucun reseau configure.";
static struct scan_config wifi_scan_config;
static xTaskHandle dns_task_handle = NULL;
static volatile bool dns_task_stop_requested = false;

static void http_send_wifi(int client);
static void http_send_patterns(int client);
static void http_send_patterns_download(int client);
static void http_handle_patterns_dat_upload(int client, const char *request, int request_len);
static void http_send_captive(int client, bool use_gzip);

static char http_chunk_buffer[768];
static size_t http_chunk_buffer_used = 0;
static int http_chunk_client = -1;

static bool station_status_needs_config_ap(STATION_STATUS status)
{
    return status == STATION_WRONG_PASSWORD ||
           status == STATION_NO_AP_FOUND ||
           status == STATION_CONNECT_FAIL ||
           status == STATION_IDLE;
}

static void wifi_enable_config_ap(const char *reason)
{
    wifi_set_opmode_current(STATIONAP_MODE);
    wifi_softap_dhcps_start();
    config_ap_active = true;
    web_captive_dns_start();
    WEB_LOG("config ap enabled reason=%s", reason);
}

static void wifi_disable_config_ap(const char *reason)
{
    struct ip_info station_ip;
    memset(&station_ip, 0, sizeof(station_ip));
    wifi_get_ip_info(STATION_IF, &station_ip);
    wifi_softap_dhcps_stop();
    wifi_set_opmode_current(STATION_MODE);
    config_ap_active = false;
    web_captive_dns_stop();
    WEB_LOG("config ap disabled reason=%s station_ip=%s", reason, ipaddr_ntoa(&station_ip.ip));
}

static int wifi_scan_result_index(const char *ssid)
{
    for (uint8_t i = 0; i < wifi_scan_count; i++) {
        if (strcmp(wifi_scan_results[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static int wifi_scan_weakest_index(void)
{
    if (wifi_scan_count == 0) {
        return -1;
    }

    int weakest = 0;
    for (uint8_t i = 1; i < wifi_scan_count; i++) {
        if (wifi_scan_results[i].rssi < wifi_scan_results[weakest].rssi) {
            weakest = i;
        }
    }
    return weakest;
}

static void wifi_scan_sort_by_rssi_desc(void)
{
    for (uint8_t i = 0; i < wifi_scan_count; i++) {
        uint8_t best = i;
        for (uint8_t j = i + 1; j < wifi_scan_count; j++) {
            if (wifi_scan_results[j].rssi > wifi_scan_results[best].rssi) {
                best = j;
            }
        }
        if (best != i) {
            wifi_scan_result_t tmp = wifi_scan_results[i];
            wifi_scan_results[i] = wifi_scan_results[best];
            wifi_scan_results[best] = tmp;
        }
    }
}

static const char *station_status_text(void)
{
    switch (wifi_station_get_connect_status()) {
    case STATION_IDLE:
        return "idle";
    case STATION_CONNECTING:
        return "connexion";
    case STATION_WRONG_PASSWORD:
        return "mot de passe incorrect";
    case STATION_NO_AP_FOUND:
        return "reseau introuvable";
    case STATION_CONNECT_FAIL:
        return "echec connexion";
    case STATION_GOT_IP:
        return "connecte";
    default:
        return "inconnu";
    }
}

static const char *scan_status_text(void)
{
    if (wifi_scan_running || wifi_scan_requested) {
        return "Scan en cours";
    }
    if (wifi_scan_count == 0) {
        return "Aucun reseau scanne";
    }
    return "Scan termine";
}

static void wifi_scan_done_cb(void *arg, STATUS status)
{
    wifi_scan_count = 0;
    wifi_scan_running = false;

    WEB_LOG("wifi scan done callback status=%d arg=%p", status, arg);

    if (status != OK) {
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "Scan WiFi echoue.");
        WEB_LOG("wifi scan failed status=%d", status);
        return;
    }

    if (arg == NULL) {
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "Aucun reseau detecte.");
        WEB_LOG("wifi scan complete empty result list");
        return;
    }

    struct bss_info *bss = (struct bss_info *)arg;
    while (bss != NULL) {
        uint8_t len = bss->ssid_len;
        if (len == 0 && bss->ssid[0] != '\0') {
            len = (uint8_t)strnlen((char *)bss->ssid, sizeof(bss->ssid));
            WEB_DEBUG_LOG("scan ssid_len fallback len=%d", len);
        }
        if (len > 32) {
            len = 32;
        }

        WEB_DEBUG_LOG("raw scan result len=%d channel=%d rssi=%d auth=%d hidden=%d",
                bss->ssid_len, bss->channel, bss->rssi, bss->authmode, bss->is_hidden);

        if (len > 0) {
            wifi_scan_result_t candidate;
            memset(&candidate, 0, sizeof(candidate));
            memcpy(candidate.ssid, bss->ssid, len);
            candidate.ssid[len] = '\0';
            candidate.rssi = bss->rssi;
            candidate.authmode = bss->authmode;
            candidate.channel = bss->channel;

            int existing = wifi_scan_result_index(candidate.ssid);
            if (existing >= 0) {
                if (candidate.rssi > wifi_scan_results[existing].rssi) {
                    wifi_scan_results[existing] = candidate;
                }
            } else if (wifi_scan_count < WIFI_SCAN_MAX_RESULTS) {
                wifi_scan_results[wifi_scan_count++] = candidate;
            } else {
                int weakest = wifi_scan_weakest_index();
                if (weakest >= 0 && candidate.rssi > wifi_scan_results[weakest].rssi) {
                    wifi_scan_results[weakest] = candidate;
                }
            }
        }

        bss = STAILQ_NEXT(bss, next);
    }

    wifi_scan_sort_by_rssi_desc();

    WEB_LOG("wifi scan complete count=%d", wifi_scan_count);
    snprintf(wifi_status_message, sizeof(wifi_status_message),
             "%d reseau(x) detecte(s).", wifi_scan_count);
}

static void wifi_start_scan(void)
{
    if (wifi_scan_running) {
        WEB_LOG("wifi scan already running");
        return;
    }

    wifi_scan_requested = false;
    wifi_set_opmode_current(STATIONAP_MODE);
    wifi_scan_count = 0;
    memset(wifi_scan_results, 0, sizeof(wifi_scan_results));
    memset(&wifi_scan_config, 0, sizeof(wifi_scan_config));
    wifi_scan_config.ssid = NULL;
    wifi_scan_config.bssid = NULL;
    wifi_scan_config.channel = 0;
    wifi_scan_config.show_hidden = 1;

    snprintf(wifi_status_message, sizeof(wifi_status_message),
             "Scan WiFi en cours...");

    WEB_LOG("wifi scan start global channel=%d show_hidden=%d status=%s",
            wifi_scan_config.channel, wifi_scan_config.show_hidden, station_status_text());
    wifi_scan_running = wifi_station_scan(&wifi_scan_config, wifi_scan_done_cb);
    if (!wifi_scan_running) {
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "Impossible de demarrer le scan WiFi.");
        WEB_LOG("wifi scan start failed");
    } else {
        WEB_LOG("wifi scan started global");
    }
}

static void wifi_request_scan(const char *reason)
{
    if (wifi_scan_running || wifi_scan_requested) {
        WEB_LOG("wifi scan request ignored reason=%s running=%d requested=%d",
                reason, wifi_scan_running, wifi_scan_requested);
        return;
    }

    snprintf(wifi_status_message, sizeof(wifi_status_message),
             "Scan WiFi demande...");
    wifi_scan_requested = true;
    WEB_LOG("wifi scan requested reason=%s", reason);
}

static void wifi_connect_to(const char *ssid, const char *password)
{
    struct station_config config;
    memset(&config, 0, sizeof(config));

    strncpy((char *)config.ssid, ssid, sizeof(config.ssid));
    strncpy((char *)config.password, password, sizeof(config.password));
    config.bssid_set = 0;

    wifi_station_disconnect();
    WEB_LOG("wifi selected ssid=%s pass_len=%u", ssid, (unsigned)strlen(password));
    if (wifi_station_set_config(&config)) {
        wifi_station_connect();
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "Connexion demandee a %s.", ssid);
        WEB_LOG("wifi connect requested ssid=%s", ssid);
    } else {
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "Erreur configuration WiFi.");
        WEB_LOG("wifi config failed ssid=%s", ssid);
    }
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t out = 0;

    while (*src != '\0' && out + 1 < dst_len) {
        if (*src == '+') {
            dst[out++] = ' ';
            src++;
        } else if (*src == '%' && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0) {
            dst[out++] = (char)((hex_value(src[1]) << 4) | hex_value(src[2]));
            src += 3;
        } else {
            dst[out++] = *src++;
        }
    }

    dst[out] = '\0';
}

static bool query_value(const char *query, const char *key, char *dst, size_t dst_len)
{
    size_t key_len = strlen(key);
    const char *p = query;

    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *value = p + key_len + 1;
            const char *end = strchr(value, '&');
            char encoded[96];
            size_t len = end == NULL ? strlen(value) : (size_t)(end - value);

            if (len >= sizeof(encoded)) {
                len = sizeof(encoded) - 1;
            }

            memcpy(encoded, value, len);
            encoded[len] = '\0';
            url_decode(dst, dst_len, encoded);
            return true;
        }

        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }

    if (dst_len > 0) {
        dst[0] = '\0';
    }
    return false;
}

static void http_send(int client, const char *s)
{
    const char *p = s;
    int remaining = (int)strlen(s);
    int total = remaining;
    int written = 0;
    int retries = 0;

    while (remaining > 0) {
        int chunk = remaining > 1024 ? 1024 : remaining;
        int sent = send(client, p, chunk, 0);
        if (sent < 0) {
            if (retries < 3) {
                retries++;
                vTaskDelay(ms_to_ticks_min1(10));
                continue;
            }
            WEB_LOG("http send failed sent_total=%d expected=%d remaining=%d errno=%d",
                    written, total, remaining, errno);
            return;
        }
        if (sent == 0) {
            WEB_DEBUG_LOG("http send closed-by-peer sent_total=%d expected=%d remaining=%d",
                          written, total, remaining);
            return;
        }

        retries = 0;
        p += sent;
        remaining -= sent;
        written += sent;
    }

    WEB_DEBUG_LOG("http send complete bytes=%d", written);
}

static void http_send_bytes(int client, const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    int remaining = (int)len;
    int retries = 0;

    while (remaining > 0) {
        int sent = send(client, (const char *)p, remaining, 0);
        if (sent < 0) {
            if (retries < 3) {
                retries++;
                vTaskDelay(ms_to_ticks_min1(10));
                continue;
            }
            WEB_LOG("http send bytes failed remaining=%d errno=%d", remaining, errno);
            return;
        }
        if (sent == 0) {
            return;
        }
        retries = 0;
        p += sent;
        remaining -= sent;
    }
}

static void http_send_response(int client,
                               const char *status,
                               const char *content_type,
                               const char *body)
{
    char header[192];
    size_t body_len = strlen(body);

    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             status, content_type, (int)body_len);

    WEB_DEBUG_LOG("http response status=%s type=%s length=%d",
            status, content_type, (int)body_len);
    http_send(client, header);
    http_send(client, body);
}

static void http_send_chunked_start(int client, const char *status, const char *content_type)
{
    char header[224];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\n"
             "Content-Type: %s\r\n"
             "Transfer-Encoding: chunked\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             status, content_type);
    http_send(client, header);
    http_chunk_client = client;
    http_chunk_buffer_used = 0;
}

static void http_send_chunk_payload(int client, const char *data, size_t len)
{
    char len_header[16];
    snprintf(len_header, sizeof(len_header), "%x\r\n", (unsigned)len);
    http_send(client, len_header);
    if (len > 0) {
        const char *p = data;
        int remaining = (int)len;
        while (remaining > 0) {
            int sent = send(client, p, remaining, 0);
            if (sent <= 0) {
                break;
            }
            p += sent;
            remaining -= sent;
        }
    }
    http_send(client, "\r\n");
}

static void http_flush_chunk_buffer(int client)
{
    if (http_chunk_client != client || http_chunk_buffer_used == 0) {
        return;
    }
    http_send_chunk_payload(client, http_chunk_buffer, http_chunk_buffer_used);
    http_chunk_buffer_used = 0;
}

static void http_send_chunk_raw(int client, const char *data, size_t len)
{
    if (http_chunk_client != client) {
        http_send_chunk_payload(client, data, len);
        return;
    }

    if (len >= sizeof(http_chunk_buffer)) {
        http_flush_chunk_buffer(client);
        http_send_chunk_payload(client, data, len);
        return;
    }

    if (http_chunk_buffer_used + len > sizeof(http_chunk_buffer)) {
        http_flush_chunk_buffer(client);
    }

    memcpy(http_chunk_buffer + http_chunk_buffer_used, data, len);
    http_chunk_buffer_used += len;
}

static void http_send_chunk(int client, const char *data)
{
    http_send_chunk_raw(client, data, strlen(data));
}

static void http_send_chunkf(int client, const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (written <= 0) {
        return;
    }
    if ((size_t)written >= sizeof(buf)) {
        written = (int)sizeof(buf) - 1;
    }
    http_send_chunk_raw(client, buf, (size_t)written);
}

static void http_send_chunked_end(int client)
{
    http_flush_chunk_buffer(client);
    http_send(client, "0\r\n\r\n");
    if (http_chunk_client == client) {
        http_chunk_client = -1;
        http_chunk_buffer_used = 0;
    }
}

static void http_send_chunk_escaped(int client, const char *s)
{
    if (s == NULL) {
        return;
    }

    char out[192];
    size_t used = 0;

    while (*s != '\0') {
        const char *entity = NULL;
        size_t entity_len = 0;

        switch (*s) {
        case '&':
            entity = "&amp;";
            entity_len = 5;
            break;
        case '<':
            entity = "&lt;";
            entity_len = 4;
            break;
        case '>':
            entity = "&gt;";
            entity_len = 4;
            break;
        case '"':
            entity = "&quot;";
            entity_len = 6;
            break;
        default:
            if (used + 1 >= sizeof(out)) {
                out[used] = '\0';
                http_send_chunk(client, out);
                used = 0;
            }
            out[used++] = *s;
            s++;
            continue;
        }

        if (used > 0) {
            http_send_chunk_raw(client, out, used);
            used = 0;
        }
        http_send_chunk_raw(client, entity, entity_len);
        s++;
    }

    if (used > 0) {
        http_send_chunk_raw(client, out, used);
    }
}

static void http_send_page_start(int client, const char *title, const char *extra_head)
{
    http_send_chunk(client, "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>");
    http_send_chunk_escaped(client, title);
    http_send_chunk(client, "</title>");
    if (extra_head != NULL) {
        http_send_chunk(client, extra_head);
    }
    http_send_chunk(client,
                    "<style>"
                    "body{font-family:Arial,sans-serif;margin:24px;max-width:720px;background:#f6f7f9;color:#15171a}"
                    "main{background:#fff;border:1px solid #d8dde3;border-radius:8px;padding:18px}"
                    "a,button{display:inline-block;margin:6px 6px 6px 0;padding:10px 14px;border-radius:6px;border:0;background:#1f6feb;color:#fff;text-decoration:none;font-size:16px}"
                    ".off{background:#b42318}.muted{color:#5b6470}.field{margin:12px 0}input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px}"
                    "</style></head><body><main><h1>");
    http_send_chunk_escaped(client, title);
    http_send_chunk(client, "</h1>");
}

static void http_send_page_end(int client)
{
    http_send_chunk(client, "</main></body></html>");
}

static void http_send_home(int client)
{
    struct ip_info station_ip;
    memset(&station_ip, 0, sizeof(station_ip));
    wifi_get_ip_info(STATION_IF, &station_ip);
    http_send_chunked_start(client, "200 OK", "text/html; charset=utf-8");
    http_send_page_start(client, "Laser Cat Toy", NULL);
    http_send_chunkf(client,
                     "<p>Jouet: <strong>%s</strong></p>"
                     "<p>Laser: <strong>%s</strong></p>"
                     "<p>Etat: <strong>%s</strong> / Session restante: %u s / Cooldown: %u s</p>"
                     "<p>WiFi: <strong>%s</strong></p>"
                     "<p>IP: <strong>%s</strong></p>"
                     "<p><a href='/on'>JEU ON</a><a class='off' href='/off'>JEU OFF</a></p>"
                     "<p>"
                     "<a href='/laser/test/on'>LASER ON TEST</a>"
                     "<a href='/laser/test/invert'>TEST INVERSE</a>",
                     game_is_enabled() ? "ON" : "OFF",
                     hardware_laser_is_on() ? "ON" : "OFF",
                     game_get_state_text(),
                     (unsigned)(game_get_session_remaining_ms() / 1000u),
                     (unsigned)(game_get_cooldown_remaining_ms() / 1000u),
                     station_status_text(),
                     ipaddr_ntoa(&station_ip.ip));
#if DEBUG_HARDWARE_ENABLED
    http_send_chunk(client, "<a href='/laser/pulse?ms=1000'>Pulse laser</a>");
#endif
    http_send_chunk(client, "<a class='off' href='/laser/off'>LASER OFF</a><a href='/patterns'>Patterns</a></p>");
    http_send_page_end(client);
    http_send_chunked_end(client);
}

static bool http_request_accepts_gzip(const char *request)
{
    const char *p = strstr(request, "\nAccept-Encoding:");
    if (p == NULL) {
        p = strstr(request, "\naccept-encoding:");
    }
    if (p == NULL) {
        return false;
    }
    const char *line_end = strchr(p, '\n');
    if (line_end == NULL) {
        line_end = p + strlen(p);
    }
    const char *g = strstr(p, "gzip");
    return g != NULL && g < line_end;
}

static void http_send_captive(int client, bool use_gzip)
{
    WEB_LOG("captive page served");
    if (use_gzip) {
        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Encoding: gzip\r\n"
                 "Vary: Accept-Encoding\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-store\r\n"
                 "\r\n",
                 (unsigned)CAPTIVE_PAGE_GZIP_LEN);
        http_send(client, header);
        http_send_bytes(client, captive_page_gzip, CAPTIVE_PAGE_GZIP_LEN);
        return;
    }

    const char *body =
        "<!doctype html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>LaserCatToy</title>"
        "</head><body>"
        "<h1>LaserCatToy</h1>"
        "<p>Portail de configuration.</p>"
        "<p><a href='/wifi'>Ouvrir la configuration WiFi</a></p>"
        "</body></html>";

    http_send_response(client, "200 OK", "text/html; charset=utf-8", body);
}

static void http_send_wifi(int client)
{
    const char *refresh_head = (wifi_scan_running || wifi_scan_requested)
                                   ? "<meta http-equiv='refresh' content='2;url=/wifi'>"
                                   : NULL;
    http_send_chunked_start(client, "200 OK", "text/html; charset=utf-8");
    http_send_page_start(client, "Configuration WiFi", refresh_head);
    http_send_chunkf(client,
                     "<p class='muted'>Portail ESP OK.</p>"
                     "<p>Jouet: <strong>%s</strong></p>"
                     "<p>Laser: <strong>%s</strong></p>"
                     "<p>Etat: <strong>%s</strong> / Session restante: %u s / Cooldown: %u s</p>"
                     "<p><a href='/on'>JEU ON</a><a class='off' href='/off'>JEU OFF</a></p>"
                     "<p>"
                     "<a href='/laser/test/on'>LASER ON TEST</a>"
                     "<a href='/laser/test/invert'>TEST INVERSE</a>",
                     game_is_enabled() ? "ON" : "OFF",
                     hardware_laser_is_on() ? "ON" : "OFF",
                     game_get_state_text(),
                     (unsigned)(game_get_session_remaining_ms() / 1000u),
                     (unsigned)(game_get_cooldown_remaining_ms() / 1000u));
#if DEBUG_HARDWARE_ENABLED
    http_send_chunk(client, "<a href='/laser/pulse?ms=1000'>Pulse laser</a>");
#endif
    http_send_chunkf(client,
                     "<a class='off' href='/laser/off'>LASER OFF</a><a href='/patterns'>Patterns</a></p>"
                     "<p>Station: %s</p>"
                     "<p>Scan: %s</p>"
                     "<p><a href='/scan'>Scanner les reseaux</a></p>"
                     "<form action='/connect' method='get'>"
                     "<div class='field'><label>Reseau detecte</label><select name='ssid'>"
                     "<option value=''>Selectionner un reseau</option>",
                     station_status_text(),
                     scan_status_text());

    for (uint8_t i = 0; i < wifi_scan_count; i++) {
        WEB_DEBUG_LOG("wifi page option ssid=%s channel=%d rssi=%d",
                      wifi_scan_results[i].ssid, wifi_scan_results[i].channel, wifi_scan_results[i].rssi);
        http_send_chunk(client, "<option value=\"");
        http_send_chunk_escaped(client, wifi_scan_results[i].ssid);
        http_send_chunk(client, "\">");
        http_send_chunk_escaped(client, wifi_scan_results[i].ssid);
        http_send_chunkf(client, " (ch %d, %d dBm)</option>",
                         wifi_scan_results[i].channel, wifi_scan_results[i].rssi);
    }

    http_send_chunk(client,
                    "</select></div>"
                    "<div class='field'><label>SSID manuel</label><input name='manual_ssid' placeholder='SSID'></div>"
                    "<div class='field'><label>Mot de passe</label><input name='pass' type='password' placeholder='Mot de passe'></div>"
                    "<button type='submit'>Connecter</button>"
                    "</form>"
                    "<p><a href='/wifi'>Rafraichir</a><a href='/patterns'>Patterns</a></p>");
    http_send_page_end(client);
    http_send_chunked_end(client);
}

static void http_send_patterns(int client)
{
    const pattern_pack_t *pack = game_get_pattern_pack();
    uint16_t step_count = 0;

    if (pack != NULL) {
        for (uint16_t i = 0; i < pack->pattern_count; i++) {
            step_count += pack->patterns[i].step_count;
        }
    }

    http_send_chunked_start(client, "200 OK", "text/html; charset=utf-8");
    http_send_page_start(client, "Patterns DAT", NULL);
    http_send_chunk(client, "<p>Etat: <strong>");
    http_send_chunk_escaped(client, game_get_pattern_status());
    http_send_chunk(client, "</strong></p><p>Source: <strong>");
    http_send_chunk_escaped(client, pack == NULL || pack->source_name == NULL ? "aucune" : pack->source_name);
    http_send_chunkf(client,
                     "</strong></p>"
                     "<p>Etat: <strong>%s</strong> / Session restante: %u s / Cooldown: %u s</p>"
                     "<p>Patterns: %u / Steps: %u / Capture every: %u</p>"
                     "<p>Selection: <strong>",
                     game_get_state_text(),
                     (unsigned)(game_get_session_remaining_ms() / 1000u),
                     (unsigned)(game_get_cooldown_remaining_ms() / 1000u),
                     pack == NULL ? 0 : pack->pattern_count,
                     step_count,
                     pack == NULL ? 0 : pack->capture_every);
    http_send_chunk_escaped(client, game_get_selected_pattern_id());
    http_send_chunk(client,
                    "</strong></p>"
                    "<form action='/patterns/select' method='get'>"
                    "<div class='field'><label>Pattern a jouer</label><select name='index'>");
    int16_t selected = game_get_selected_pattern();
    http_send_chunkf(client, "<option value='-1'%s>Auto / aleatoire pondere</option>",
                     selected < 0 ? " selected" : "");
    if (pack != NULL) {
        for (uint16_t i = 0; i < pack->pattern_count; i++) {
            http_send_chunkf(client, "<option value='%d'%s>", i, selected == (int16_t)i ? " selected" : "");
            http_send_chunk_escaped(client, pack->patterns[i].id);
            http_send_chunk(client, " - ");
            http_send_chunk_escaped(client, pack->patterns[i].name);
            http_send_chunk(client, "</option>");
        }
    }
    http_send_chunkf(client,
                     "</select></div><button type='submit'>Appliquer le pattern</button></form>"
                     "<form action='/patterns/speed' method='get'>"
                     "<div class='field'><label>Vitesse globale: <strong>%u%%</strong></label>"
                     "<input name='value' type='number' min='%u' max='%u' step='5' value='%u'></div>"
                     "<button type='submit'>Appliquer la vitesse</button>"
                     "<a href='/patterns/speed?value=75'>75%%</a>"
                     "<a href='/patterns/speed?value=100'>100%%</a>"
                     "<a href='/patterns/speed?value=150'>150%%</a>"
                     "</form>",
                     game_get_speed_percent(),
                     PATTERN_SPEED_MIN_PERCENT,
                     PATTERN_SPEED_MAX_PERCENT,
                     game_get_speed_percent());
    http_send_chunk(client,
                    "<div class='field'><input id='patterns_file' type='file' accept='.dat,application/octet-stream'></div>"
                    "<button id='upload' type='button'>Uploader et activer</button>"
                    "<p id='result' class='muted'></p>"
                    "<script>"
                    "const f=document.getElementById('patterns_file'),r=document.getElementById('result');"
                    "document.getElementById('upload').onclick=async()=>{"
                    "if(!f.files.length){r.textContent='Selectionner un fichier DAT';return;}"
                    "r.textContent='Validation et activation...';"
                    "try{const x=await fetch('/patterns/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f.files[0]});"
                    "r.textContent=await x.text();if(x.ok)setTimeout(()=>location.reload(),900);}"
                    "catch(e){r.textContent='Echec upload';}"
                    "};"
                    "</script>");
    http_send_chunkf(client, "<p class='muted'>SPIFFS: %s</p>", pattern_store_get_status());
    http_send_chunk(client, "<p><a href='/patterns/download'>Telecharger le pack actif</a><a href='/'>Accueil</a></p>");
    http_send_page_end(client);
    http_send_chunked_end(client);
}

static const char *step_type_name(pattern_step_type_t type)
{
    switch (type) {
    case STEP_HOLD:
        return "hold";
    case STEP_MOVE:
        return "move";
    case STEP_JITTER:
        return "jitter";
    case STEP_OFF_HOLD:
        return "off_hold";
    case STEP_OFF_MOVE:
        return "off_move";
    default:
        return "hold";
    }
}

static bool step_has_json_position(pattern_step_type_t type)
{
    return type == STEP_HOLD ||
           type == STEP_MOVE ||
           type == STEP_JITTER ||
           type == STEP_OFF_MOVE;
}

static void http_send_json_string(int client, const char *s)
{
    http_send(client, "\"");
    while (s != NULL && *s != '\0') {
        char chunk[8];
        switch (*s) {
        case '\\':
            http_send(client, "\\\\");
            break;
        case '"':
            http_send(client, "\\\"");
            break;
        case '\n':
            http_send(client, "\\n");
            break;
        case '\r':
            http_send(client, "\\r");
            break;
        case '\t':
            http_send(client, "\\t");
            break;
        default:
            snprintf(chunk, sizeof(chunk), "%c", *s);
            http_send(client, chunk);
            break;
        }
        s++;
    }
    http_send(client, "\"");
}

static void http_send_patterns_download(int client)
{
    const pattern_pack_t *pack = game_get_pattern_pack();
    char chunk[256];

    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n";
    http_send(client, header);

    if (pack == NULL) {
        http_send(client, "{}\n");
        return;
    }

    snprintf(chunk, sizeof(chunk),
             "{\"schema\":\"%s\",\"runtime\":{\"selection_mode\":\"weighted_random\",\"capture_every\":%u},\"patterns\":[",
             PATTERN_SCHEMA, pack->capture_every);
    http_send(client, chunk);

    for (uint16_t i = 0; i < pack->pattern_count; i++) {
        const pattern_t *pattern = &pack->patterns[i];
        if (pattern->steps == NULL) {
            http_send(client, "{\"error\":\"download indisponible pour pack charge a la demande\"}\n");
            return;
        }
        snprintf(chunk, sizeof(chunk),
                 "%s{\"id\":",
                 i == 0 ? "" : ",");
        http_send(client, chunk);
        http_send_json_string(client, pattern->id);
        http_send(client, ",\"name\":");
        http_send_json_string(client, pattern->name);
        snprintf(chunk, sizeof(chunk), ",\"weight\":%u,\"steps\":[",
                 pattern->weight);
        http_send(client, chunk);

        for (uint16_t j = 0; j < pattern->step_count; j++) {
            const pattern_step_t *step = &pattern->steps[j];
            snprintf(chunk, sizeof(chunk),
                     "%s{\"type\":\"%s\",\"laser\":%s,\"duration_ms\":%u",
                     j == 0 ? "" : ",",
                     step_type_name(step->type),
                     step->laser ? "true" : "false",
                     step->duration_ms);
            http_send(client, chunk);

            if (step_has_json_position(step->type)) {
                snprintf(chunk, sizeof(chunk), ",\"x\":%.3f,\"y\":%.3f",
                         (double)step->x / 1000.0,
                         (double)step->y / 1000.0);
                http_send(client, chunk);
            }
            if (step->type == STEP_JITTER) {
                snprintf(chunk, sizeof(chunk), ",\"amplitude\":%.3f",
                         (double)step->amplitude / 1000.0);
                http_send(client, chunk);
            }
            http_send(client, "}");
        }
        http_send(client, "]}");
    }

    http_send(client, "]}\n");
}

static void http_send_no_content(int client)
{
    const char *header =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n";

    WEB_DEBUG_LOG("http response status=204 No Content length=0");
    http_send(client, header);
}

static void http_redirect(int client, const char *location)
{
    char header[160];

    snprintf(header, sizeof(header),
             "HTTP/1.1 302 Found\r\n"
             "Location: %s\r\n"
             "Content-Length: 0\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             location);
    WEB_LOG("http redirect status=302 location=%s", location);
    http_send(client, header);
}

static void http_extract_host(const char *request, char *host, size_t host_len)
{
    const char *p = strstr(request, "\nHost:");
    if (p == NULL) {
        p = strstr(request, "\nhost:");
    }

    if (host_len == 0) {
        return;
    }
    host[0] = '\0';

    if (p == NULL) {
        return;
    }

    p += 6;
    while (*p == ' ') {
        p++;
    }

    const char *end = strchr(p, '\r');
    if (end == NULL) {
        end = strchr(p, '\n');
    }
    if (end == NULL) {
        end = p + strlen(p);
    }

    size_t len = (size_t)(end - p);
    if (len >= host_len) {
        len = host_len - 1;
    }

    memcpy(host, p, len);
    host[len] = '\0';
}

static void http_handle_connect_request(int client, const char *query)
{
    char ssid[33];
    char manual_ssid[33];
    char password[65];

    query_value(query, "ssid", ssid, sizeof(ssid));
    query_value(query, "manual_ssid", manual_ssid, sizeof(manual_ssid));
    query_value(query, "pass", password, sizeof(password));

    if (manual_ssid[0] != '\0') {
        strncpy(ssid, manual_ssid, sizeof(ssid));
        ssid[sizeof(ssid) - 1] = '\0';
        WEB_LOG("manual ssid provided ssid=%s", ssid);
    } else {
        WEB_LOG("listed ssid selected ssid=%s", ssid);
    }

    if (ssid[0] != '\0') {
        wifi_connect_to(ssid, password);
    } else {
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "SSID vide, connexion ignoree.");
        WEB_LOG("wifi connect ignored empty ssid");
    }

    http_redirect(client, "/wifi");
}

static void http_handle_patterns_select_request(int client, const char *query)
{
    char value[16];

    WEB_LOG("pattern select request query=%s current_index=%d current_id=%s",
            query,
            game_get_selected_pattern(),
            game_get_selected_pattern_id());
    query_value(query, "index", value, sizeof(value));
    if (value[0] != '\0') {
        int requested_index = atoi(value);
        if (!game_set_selected_pattern((int16_t)requested_index)) {
            WEB_LOG("pattern select rejected index=%d current_index=%d current_id=%s",
                    requested_index,
                    game_get_selected_pattern(),
                    game_get_selected_pattern_id());
        } else {
            WEB_LOG("pattern select applied requested=%d current_index=%d current_id=%s",
                    requested_index,
                    game_get_selected_pattern(),
                    game_get_selected_pattern_id());
        }
    } else {
        WEB_LOG("pattern select ignored missing index query=%s", query);
    }

    http_redirect(client, "/patterns");
}

static void http_handle_patterns_speed_request(int client, const char *query)
{
    char value[16];

    WEB_LOG("pattern speed request query=%s current_speed=%u",
            query,
            game_get_speed_percent());
    query_value(query, "value", value, sizeof(value));
    if (value[0] != '\0') {
        int speed = atoi(value);
        if (speed > 0) {
            game_set_speed_percent((uint16_t)speed);
            WEB_LOG("pattern speed applied requested=%d current_speed=%u",
                    speed,
                    game_get_speed_percent());
        } else {
            WEB_LOG("pattern speed ignored invalid value=%s", value);
        }
    } else {
        WEB_LOG("pattern speed ignored missing value query=%s", query);
    }

    http_redirect(client, "/patterns");
}

static void http_handle_laser_pulse_request(int client, const char *query)
{
#if DEBUG_HARDWARE_ENABLED
    char value[16];
    uint16_t duration_ms = LASER_PULSE_DEFAULT_MS;

    query_value(query, "ms", value, sizeof(value));
    if (value[0] != '\0') {
        int parsed = atoi(value);
        if (parsed > 0) {
            duration_ms = (uint16_t)parsed;
        }
    }

    game_laser_pulse(duration_ms);
    WEB_LOG("laser pulse requested ms=%u state=%s",
            duration_ms,
            game_get_state_text());
    http_redirect(client, "/");
#else
    (void)query;
    http_send_response(client, "404 Not Found", "text/plain; charset=utf-8",
                       "Debug hardware desactive.\n");
#endif
}

static const char *http_find_header_value(const char *request, const char *name)
{
    size_t name_len = strlen(name);
    const char *p = request;

    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        if (strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            p += name_len + 1;
            while (*p == ' ') {
                p++;
            }
            return p;
        }
    }

    return NULL;
}

static bool http_content_length(const char *request, uint32_t *content_length)
{
    const char *value = http_find_header_value(request, "Content-Length");
    uint32_t parsed = 0;

    if (value == NULL || *value < '0' || *value > '9') {
        return false;
    }

    while (*value >= '0' && *value <= '9') {
        parsed = (parsed * 10u) + (uint32_t)(*value - '0');
        value++;
    }

    *content_length = parsed;
    return true;
}

static int http_header_body_offset(const char *request, int request_len)
{
    for (int i = 0; i + 3 < request_len; i++) {
        if (request[i] == '\r' && request[i + 1] == '\n' &&
            request[i + 2] == '\r' && request[i + 3] == '\n') {
            return i + 4;
        }
    }

    return -1;
}

static void http_handle_patterns_dat_upload(int client, const char *request, int request_len)
{
    uint32_t content_length = 0;
    int body_offset = http_header_body_offset(request, request_len);
    char message[96];
    char load_message[96];
    const pattern_pack_t *pack = NULL;

    if (body_offset < 0 || !http_content_length(request, &content_length)) {
        http_send_response(client, "400 Bad Request", "text/plain; charset=utf-8", "Headers DAT invalides.\n");
        return;
    }
    if (!pattern_store_begin_upload(content_length, message, sizeof(message))) {
        http_send_response(client, "400 Bad Request", "text/plain; charset=utf-8", message);
        return;
    }

    if (content_length == 0) {
        http_send_response(client, "413 Payload Too Large", "text/plain; charset=utf-8",
                           "DAT vide.\n");
        return;
    }

    uint32_t received = 0;
    int initial_body_len = request_len - body_offset;
    if (initial_body_len > 0) {
        if ((uint32_t)initial_body_len > content_length) {
            initial_body_len = (int)content_length;
        }
        if (!pattern_store_write_upload_chunk((const uint8_t *)(request + body_offset), (uint32_t)initial_body_len, message, sizeof(message))) {
            http_send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8", message);
            return;
        }
        received = (uint32_t)initial_body_len;
    }

    while (received < content_length) {
        uint32_t remaining = content_length - received;
        int to_read = remaining > 1024u ? 1024 : (int)remaining;
        uint8_t chunk[1024];
        int n = recv(client, (char *)chunk, to_read, 0);
        if (n <= 0) {
            http_send_response(client, "400 Bad Request", "text/plain; charset=utf-8",
                               "Connexion fermee pendant upload DAT.\n");
            return;
        }
        if (!pattern_store_write_upload_chunk(chunk, (uint32_t)n, message, sizeof(message))) {
            http_send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8", message);
            return;
        }
        received += (uint32_t)n;
    }

    if (!pattern_store_finish_upload(message, sizeof(message))) {
        http_send_response(client, "500 Internal Server Error", "text/plain; charset=utf-8", message);
        return;
    }
    if (!pattern_store_load_active_pack(&pack, load_message, sizeof(load_message)) || pack == NULL) {
        http_send_response(client, "400 Bad Request", "text/plain; charset=utf-8", load_message);
        return;
    }
    game_use_pattern_pack(pack, load_message);
    http_send_response(client, "200 OK", "text/plain; charset=utf-8", game_get_pattern_status());
}

static void http_handle_request(int client, char *request, int request_len)
{
    char path[384];
    char method[8];
    char host[96];
    bool use_gzip = http_request_accepts_gzip(request);
    char *start = strchr(request, ' ');
    char *end;

    http_extract_host(request, host, sizeof(host));
    method[0] = '\0';
    sscanf(request, "%7s", method);

    if (start == NULL) {
        http_send_captive(client, use_gzip);
        return;
    }

    start++;
    end = strchr(start, ' ');
    if (end == NULL) {
        http_send_captive(client, use_gzip);
        return;
    }

    size_t len = (size_t)(end - start);
    if (len >= sizeof(path)) {
        len = sizeof(path) - 1;
    }
    memcpy(path, start, len);
    path[len] = '\0';

    if (strcmp(method, "POST") == 0 && strcmp(path, "/patterns/upload") == 0) {
        WEB_LOG("patterns.dat upload requested");
        game_set_enabled(false);
        hardware_laser_set(false);
        http_handle_patterns_dat_upload(client, request, request_len);
    } else if (strcmp(path, "/on") == 0) {
        if (game_set_enabled(true)) {
            WEB_LOG("toy enabled from web state=%s", game_get_state_text());
        } else {
            WEB_LOG("toy start rejected state=%s cooldown_remaining_ms=%u",
                    game_get_state_text(),
                    (unsigned)game_get_cooldown_remaining_ms());
        }
        http_redirect(client, "/");
    } else if (strcmp(path, "/off") == 0) {
        game_set_enabled(false);
        WEB_LOG("toy disabled from web state=%s", game_get_state_text());
        http_redirect(client, "/");
    } else if (strncmp(path, "/laser/pulse?", 13) == 0) {
        http_handle_laser_pulse_request(client, path + 13);
    } else if (strcmp(path, "/laser/test/on") == 0) {
        game_laser_test_on();
        WEB_LOG("laser test on requested state=%s", game_get_state_text());
        http_redirect(client, "/");
    } else if (strcmp(path, "/laser/test/invert") == 0) {
        game_laser_test_on_inverted();
        WEB_LOG("laser inverted test requested state=%s", game_get_state_text());
        http_redirect(client, "/");
    } else if (strcmp(path, "/laser/on") == 0) {
        game_laser_test_on();
        WEB_LOG("legacy laser on mapped to test state=%s", game_get_state_text());
        http_redirect(client, "/");
    } else if (strcmp(path, "/laser/off") == 0) {
        game_laser_test_off();
        WEB_LOG("laser disabled from web");
        http_redirect(client, "/");
    } else if (strcmp(path, "/scan") == 0) {
        WEB_LOG("manual wifi scan requested");
        wifi_request_scan("manual");
        http_redirect(client, "/wifi");
    } else if (strncmp(path, "/connect?", 9) == 0) {
        http_handle_connect_request(client, path + 9);
    } else if (strcmp(path, "/patterns") == 0) {
        WEB_LOG("patterns page requested");
        http_send_patterns(client);
    } else if (strcmp(path, "/patterns/download") == 0) {
        WEB_LOG("patterns download requested");
        http_send_patterns_download(client);
    } else if (strncmp(path, "/patterns/select?", 17) == 0) {
        http_handle_patterns_select_request(client, path + 17);
    } else if (strncmp(path, "/patterns/speed?", 16) == 0) {
        http_handle_patterns_speed_request(client, path + 16);
    } else if (strcmp(path, "/wifi") == 0) {
        WEB_LOG("wifi page requested status=%s count=%d running=%d",
                station_status_text(), wifi_scan_count, wifi_scan_running);
        http_send_wifi(client);
    } else if (strcmp(path, "/favicon.ico") == 0) {
        WEB_LOG("favicon ignored");
        http_send_no_content(client);
    } else if (strcmp(path, "/") == 0) {
        WEB_LOG("root requested host=%s toy=%d station=%s",
                host, game_is_enabled(), station_status_text());
        if (wifi_station_get_connect_status() == STATION_GOT_IP) {
            http_send_home(client);
        } else {
            WEB_LOG("root redirected to wifi config station=%s", station_status_text());
            http_redirect(client, "/wifi");
        }
    } else if (strcmp(path, "/generate_204") == 0 ||
               strcmp(path, "/gen_204") == 0 ||
               strcmp(path, "/hotspot-detect.html") == 0 ||
               strcmp(path, "/library/test/success.html") == 0 ||
               strcmp(path, "/ncsi.txt") == 0 ||
               strcmp(path, "/connecttest.txt") == 0 ||
               strcmp(path, "/redirect") == 0) {
        WEB_LOG("captive probe host=%s path=%s -> serve captive landing", host, path);
        http_send_captive(client, use_gzip);
    } else {
        WEB_LOG("unknown path served as captive host=%s path=%s", host, path);
        http_send_captive(client, use_gzip);
    }
}

static void http_close_client(int client)
{
    WEB_DEBUG_LOG("http client shutdown");
    shutdown(client, SHUT_RDWR);
    closesocket(client);
    WEB_DEBUG_LOG("http client closed");
}

static void http_set_client_timeouts(int client)
{
    struct timeval timeout;
    timeout.tv_sec = HTTP_SOCKET_TIMEOUT_MS / 1000;
    timeout.tv_usec = (HTTP_SOCKET_TIMEOUT_MS % 1000) * 1000;

    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int dns_question_end(const uint8_t *packet, int len)
{
    int pos = 12;

    while (pos < len && packet[pos] != 0) {
        pos += packet[pos] + 1;
    }

    if (pos + 5 > len) {
        return -1;
    }

    return pos + 5;
}

static int dns_open_server(void)
{
    int server = socket(AF_INET, SOCK_DGRAM, 0);
    if (server < 0) {
        WEB_LOG("dns socket failed");
        return -1;
    }

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0;
    addr.sin_port = htons(DNS_PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        WEB_LOG("dns bind failed");
        closesocket(server);
        return -1;
    }

    WEB_LOG("captive dns ready");
    return server;
}

void web_dns_server_task(void *arg)
{
    (void)arg;

    int server = dns_open_server();
    if (server < 0) {
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (dns_task_stop_requested) {
            break;
        }

        uint8_t packet[256];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(server, packet, sizeof(packet), 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < 12) {
            continue;
        }

        int qend = dns_question_end(packet, len);
        if (qend < 0 || qend + 16 > (int)sizeof(packet)) {
            continue;
        }

        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0x00;
        packet[7] = 0x01;
        packet[8] = 0x00;
        packet[9] = 0x00;
        packet[10] = 0x00;
        packet[11] = 0x00;

        int pos = qend;
        packet[pos++] = 0xC0;
        packet[pos++] = 0x0C;
        packet[pos++] = 0x00;
        packet[pos++] = 0x01;
        packet[pos++] = 0x00;
        packet[pos++] = 0x01;
        packet[pos++] = 0x00;
        packet[pos++] = 0x00;
        packet[pos++] = 0x00;
        packet[pos++] = 0x3C;
        packet[pos++] = 0x00;
        packet[pos++] = 0x04;
        packet[pos++] = CONFIG_AP_IP_A;
        packet[pos++] = CONFIG_AP_IP_B;
        packet[pos++] = CONFIG_AP_IP_C;
        packet[pos++] = CONFIG_AP_IP_D;

        sendto(server, packet, pos, 0, (struct sockaddr *)&client_addr, client_len);
    }

    closesocket(server);
    dns_task_handle = NULL;
    dns_task_stop_requested = false;
    vTaskDelete(NULL);
}

void web_captive_dns_start(void)
{
    if (dns_task_handle != NULL) {
        return;
    }

    dns_task_stop_requested = false;
    if (xTaskCreate(web_dns_server_task, "dns", 768, NULL, 4, &dns_task_handle) == pdPASS) {
        WEB_LOG("captive dns task started");
    } else {
        dns_task_handle = NULL;
        WEB_LOG("captive dns task start failed");
    }
}

void web_captive_dns_stop(void)
{
    if (dns_task_handle == NULL) {
        return;
    }

    dns_task_stop_requested = true;
    for (uint8_t i = 0; i < 12 && dns_task_handle != NULL; i++) {
        vTaskDelay(ms_to_ticks_min1(100));
    }
    if (dns_task_handle != NULL) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
        dns_task_stop_requested = false;
    }
    WEB_LOG("captive dns task stopped");
}

void web_http_server_task(void *arg)
{
    (void)arg;

    vTaskDelay(ms_to_ticks_min1(800));

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        WEB_LOG("http socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0;
    addr.sin_port = htons(HTTP_PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(server, 2) != 0) {
        WEB_LOG("http bind/listen failed");
        closesocket(server);
        vTaskDelete(NULL);
        return;
    }

    WEB_LOG("http server ready path=/");

    while (true) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            vTaskDelay(ms_to_ticks_min1(50));
            continue;
        }
        http_set_client_timeouts(client);

        char request[1024];
        int n = recv(client, request, sizeof(request) - 1, 0);
        if (n > 0) {
            request[n] = '\0';
            char method[8];
            char path[96];
            method[0] = '\0';
            path[0] = '\0';
            sscanf(request, "%7s %95s", method, path);
            char host[96];
            http_extract_host(request, host, sizeof(host));
            WEB_DEBUG_LOG("http request method=%s host=%s path=%s", method, host, path);
            http_handle_request(client, request, n);
        } else {
            WEB_DEBUG_LOG("http request empty recv=%d", n);
        }

        http_close_client(client);
    }
}

void web_wifi_status_task(void *arg)
{
    (void)arg;

    STATION_STATUS last_status = STATION_IDLE;

    while (true) {
        STATION_STATUS status = wifi_station_get_connect_status();

        if (status != last_status) {
            WEB_LOG("wifi station status=%s", station_status_text());
            if (status == STATION_GOT_IP) {
                wifi_disable_config_ap("station connected");
            } else if (station_status_needs_config_ap(status)) {
                wifi_enable_config_ap("station not connected");
            }
            last_status = status;
        }

        if (wifi_scan_requested && !wifi_scan_running) {
            wifi_scan_requested = false;
            wifi_start_scan();
        }

        vTaskDelay(ms_to_ticks_min1(1000));
    }
}

void web_portal_init(void)
{
    WEB_LOG("wifi init station+ap");
    wifi_set_opmode_current(STATIONAP_MODE);
    config_ap_active = true;
    wifi_station_set_auto_connect(true);
    wifi_station_set_reconnect_policy(true);
    wifi_softap_dhcps_stop();

    struct ip_info ap_ip;
    IP4_ADDR(&ap_ip.ip, CONFIG_AP_IP_A, CONFIG_AP_IP_B, CONFIG_AP_IP_C, CONFIG_AP_IP_D);
    IP4_ADDR(&ap_ip.gw, CONFIG_AP_IP_A, CONFIG_AP_IP_B, CONFIG_AP_IP_C, CONFIG_AP_IP_D);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);

    if (wifi_set_ip_info(SOFTAP_IF, &ap_ip)) {
        WEB_LOG("softap ip configured ip=%d.%d.%d.%d",
                CONFIG_AP_IP_A, CONFIG_AP_IP_B, CONFIG_AP_IP_C, CONFIG_AP_IP_D);
    } else {
        WEB_LOG("softap ip configure failed");
    }

    struct softap_config ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strncpy((char *)ap_config.ssid, CONFIG_AP_SSID, sizeof(ap_config.ssid));
    strncpy((char *)ap_config.password, CONFIG_AP_PASSWORD, sizeof(ap_config.password));
    ap_config.ssid_len = strlen(CONFIG_AP_SSID);
    ap_config.channel = 6;
    ap_config.authmode = AUTH_WPA_WPA2_PSK;
    ap_config.ssid_hidden = 0;
    ap_config.max_connection = 4;
    ap_config.beacon_interval = 100;

    wifi_softap_set_config_current(&ap_config);
    wifi_softap_dhcps_start();
    struct ip_info actual_ip;
    memset(&actual_ip, 0, sizeof(actual_ip));
    wifi_get_ip_info(SOFTAP_IF, &actual_ip);
    WEB_LOG("config ap ready ssid=%s ip=%d.%d.%d.%d actual_ip=%s http_port=%d dns_port=%d",
            CONFIG_AP_SSID,
            CONFIG_AP_IP_A, CONFIG_AP_IP_B, CONFIG_AP_IP_C, CONFIG_AP_IP_D,
            ipaddr_ntoa(&actual_ip.ip),
            HTTP_PORT, DNS_PORT);
    web_captive_dns_start();
}
