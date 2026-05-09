#include "web.h"

#include "app_util.h"
#include "game.h"
#include "logging.h"
#include "main.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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
static char wifi_status_message[96] = "Aucun reseau configure.";
static char http_body[4096];
static struct scan_config wifi_scan_config;

static void http_send_wifi(int client);

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
    WEB_LOG("config ap enabled reason=%s", reason);
}

static void wifi_disable_config_ap(const char *reason)
{
    struct ip_info station_ip;
    memset(&station_ip, 0, sizeof(station_ip));
    wifi_get_ip_info(STATION_IF, &station_ip);
    wifi_softap_dhcps_stop();
    wifi_set_opmode_current(STATION_MODE);
    WEB_LOG("config ap disabled reason=%s station_ip=%s", reason, ipaddr_ntoa(&station_ip.ip));
}

static bool wifi_scan_result_exists(const char *ssid)
{
    for (uint8_t i = 0; i < wifi_scan_count; i++) {
        if (strcmp(wifi_scan_results[i].ssid, ssid) == 0) {
            return true;
        }
    }

    return false;
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
    while (bss != NULL && wifi_scan_count < WIFI_SCAN_MAX_RESULTS) {
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
            memset(&wifi_scan_results[wifi_scan_count], 0, sizeof(wifi_scan_results[0]));
            memcpy(wifi_scan_results[wifi_scan_count].ssid, bss->ssid, len);
            wifi_scan_results[wifi_scan_count].ssid[len] = '\0';
            if (!wifi_scan_result_exists(wifi_scan_results[wifi_scan_count].ssid)) {
                wifi_scan_results[wifi_scan_count].rssi = bss->rssi;
                wifi_scan_results[wifi_scan_count].authmode = bss->authmode;
                wifi_scan_results[wifi_scan_count].channel = bss->channel;
                WEB_LOG("scan result ssid=%s channel=%d rssi=%d auth=%d",
                        wifi_scan_results[wifi_scan_count].ssid,
                        wifi_scan_results[wifi_scan_count].channel,
                        wifi_scan_results[wifi_scan_count].rssi,
                        wifi_scan_results[wifi_scan_count].authmode);
                wifi_scan_count++;
            } else {
                WEB_DEBUG_LOG("scan duplicate ignored ssid=%s", wifi_scan_results[wifi_scan_count].ssid);
            }
        }

        bss = STAILQ_NEXT(bss, next);
    }

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
    WEB_LOG("wifi selected ssid=%s pass_len=%d", ssid, strlen(password));
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

    while (remaining > 0) {
        int sent = send(client, p, remaining, 0);
        if (sent <= 0) {
            WEB_LOG("http send failed sent_total=%d expected=%d remaining=%d",
                    written, total, remaining);
            return;
        }

        p += sent;
        remaining -= sent;
        written += sent;
    }

    WEB_DEBUG_LOG("http send complete bytes=%d", written);
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

static void appendf(char *dst, size_t dst_len, size_t *used, const char *fmt, ...)
{
    if (*used >= dst_len) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(dst + *used, dst_len - *used, fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }
    if ((size_t)written >= dst_len - *used) {
        *used = dst_len - 1;
    } else {
        *used += (size_t)written;
    }
}

static void append_escaped(char *dst, size_t dst_len, size_t *used, const char *s)
{
    while (*s != '\0') {
        switch (*s) {
        case '&':
            appendf(dst, dst_len, used, "&amp;");
            break;
        case '<':
            appendf(dst, dst_len, used, "&lt;");
            break;
        case '>':
            appendf(dst, dst_len, used, "&gt;");
            break;
        case '"':
            appendf(dst, dst_len, used, "&quot;");
            break;
        default:
            appendf(dst, dst_len, used, "%c", *s);
            break;
        }
        s++;
    }
}

static void append_page_start(char *body, size_t body_len, size_t *used, const char *title)
{
    appendf(body, body_len, used,
            "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>");
    append_escaped(body, body_len, used, title);
    appendf(body, body_len, used,
            "</title><style>"
            "body{font-family:Arial,sans-serif;margin:24px;max-width:720px;background:#f6f7f9;color:#15171a}"
            "main{background:#fff;border:1px solid #d8dde3;border-radius:8px;padding:18px}"
            "a,button{display:inline-block;margin:6px 6px 6px 0;padding:10px 14px;border-radius:6px;border:0;background:#1f6feb;color:#fff;text-decoration:none;font-size:16px}"
            ".off{background:#b42318}.muted{color:#5b6470}.field{margin:12px 0}input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px}"
            "</style></head><body><main><h1>");
    append_escaped(body, body_len, used, title);
    appendf(body, body_len, used, "</h1>");
}

static void append_page_end(char *body, size_t body_len, size_t *used)
{
    appendf(body, body_len, used, "</main></body></html>");
}

static void append_wifi_options(char *body, size_t body_len, size_t *used)
{
    for (uint8_t i = 0; i < wifi_scan_count; i++) {
        WEB_DEBUG_LOG("wifi page option ssid=%s channel=%d rssi=%d",
                wifi_scan_results[i].ssid, wifi_scan_results[i].channel, wifi_scan_results[i].rssi);
        appendf(body, body_len, used, "<option value=\"");
        append_escaped(body, body_len, used, wifi_scan_results[i].ssid);
        appendf(body, body_len, used, "\">");
        append_escaped(body, body_len, used, wifi_scan_results[i].ssid);
        appendf(body, body_len, used, " (ch %d, %d dBm)</option>",
                wifi_scan_results[i].channel, wifi_scan_results[i].rssi);
    }
}

static void http_send_home(int client)
{
    size_t used = 0;
    struct ip_info station_ip;
    memset(&station_ip, 0, sizeof(station_ip));
    wifi_get_ip_info(STATION_IF, &station_ip);

    http_body[0] = '\0';
    append_page_start(http_body, sizeof(http_body), &used, "Laser Cat Toy");
    appendf(http_body, sizeof(http_body), &used,
            "<p>Jouet: <strong>%s</strong></p>"
            "<p>WiFi: <strong>%s</strong></p>"
            "<p>IP: <strong>%s</strong></p>"
            "<p><a href='/on'>ON</a><a class='off' href='/off'>OFF</a></p>",
            game_is_enabled() ? "ON" : "OFF",
            station_status_text(),
            ipaddr_ntoa(&station_ip.ip));
    append_page_end(http_body, sizeof(http_body), &used);

    WEB_DEBUG_LOG("home page served bytes=%d", (int)strlen(http_body));
    http_send_response(client, "200 OK", "text/html; charset=utf-8", http_body);
}

static void http_send_captive(int client)
{
    WEB_LOG("captive page served");
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
    size_t used = 0;
    http_body[0] = '\0';
    append_page_start(http_body, sizeof(http_body), &used, "Configuration WiFi");
    appendf(http_body, sizeof(http_body), &used,
            "%s"
            "<p class='muted'>Portail ESP OK.</p>"
            "<p>Station: %s</p>"
            "<p>Scan: %s</p>"
            "<p><a href='/scan'>Scanner les reseaux</a></p>"
            "<form action='/connect' method='get'>"
            "<div class='field'><label>Reseau detecte</label><select name='ssid'>"
            "<option value=''>Selectionner un reseau</option>",
            (wifi_scan_running || wifi_scan_requested) ? "<meta http-equiv='refresh' content='2;url=/wifi'>" : "",
            station_status_text(),
            scan_status_text());

    append_wifi_options(http_body, sizeof(http_body), &used);

    appendf(http_body, sizeof(http_body), &used,
            "</select></div>"
            "<div class='field'><label>SSID manuel</label><input name='manual_ssid' placeholder='SSID'></div>"
            "<div class='field'><label>Mot de passe</label><input name='pass' type='password' placeholder='Mot de passe'></div>"
            "<button type='submit'>Connecter</button>"
            "</form>"
            "<p><a href='/wifi'>Rafraichir</a></p>");
    append_page_end(http_body, sizeof(http_body), &used);

    WEB_DEBUG_LOG("wifi simple page served bytes=%d scan_count=%d scan_running=%d scan_requested=%d",
            (int)strlen(http_body), wifi_scan_count, wifi_scan_running, wifi_scan_requested);
    http_send_response(client, "200 OK", "text/html; charset=utf-8", http_body);
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

static void http_handle_request(int client, char *request)
{
    char path[384];
    char host[96];
    char *start = strchr(request, ' ');
    char *end;

    http_extract_host(request, host, sizeof(host));

    if (start == NULL) {
        http_send_captive(client);
        return;
    }

    start++;
    end = strchr(start, ' ');
    if (end == NULL) {
        http_send_captive(client);
        return;
    }

    size_t len = (size_t)(end - start);
    if (len >= sizeof(path)) {
        len = sizeof(path) - 1;
    }
    memcpy(path, start, len);
    path[len] = '\0';

    if (strcmp(path, "/on") == 0) {
        game_set_enabled(true);
        WEB_LOG("toy enabled from web");
        http_redirect(client, "/");
    } else if (strcmp(path, "/off") == 0) {
        game_set_enabled(false);
        WEB_LOG("toy disabled from web");
        http_redirect(client, "/");
    } else if (strcmp(path, "/scan") == 0) {
        WEB_LOG("manual wifi scan requested");
        wifi_request_scan("manual");
        http_redirect(client, "/wifi");
    } else if (strncmp(path, "/connect?", 9) == 0) {
        http_handle_connect_request(client, path + 9);
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
        http_send_captive(client);
    } else {
        WEB_LOG("unknown path served as captive host=%s path=%s", host, path);
        http_send_captive(client);
    }
}

static void http_close_client(int client)
{
    WEB_DEBUG_LOG("http client shutdown");
    shutdown(client, SHUT_RDWR);
    closesocket(client);
    WEB_DEBUG_LOG("http client closed");
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

void web_dns_server_task(void *arg)
{
    (void)arg;

    int server = socket(AF_INET, SOCK_DGRAM, 0);
    if (server < 0) {
        WEB_LOG("dns socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0;
    addr.sin_port = htons(DNS_PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        WEB_LOG("dns bind failed");
        closesocket(server);
        vTaskDelete(NULL);
        return;
    }

    WEB_LOG("captive dns ready");

    while (true) {
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

        char request[512];
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
            http_handle_request(client, request);
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
}
