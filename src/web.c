#include "web.h"

#include "app_util.h"
#include "game.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

#include "esp_common.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#if WEB_LOG_ENABLED
#define WEB_LOG(fmt, ...) os_printf("[web] " fmt "\n", ##__VA_ARGS__)
#else
#define WEB_LOG(fmt, ...)
#endif

typedef struct {
    char ssid[33];
    sint8 rssi;
    AUTH_MODE authmode;
} wifi_scan_result_t;

static wifi_scan_result_t wifi_scan_results[WIFI_SCAN_MAX_RESULTS];
static volatile uint8_t wifi_scan_count = 0;
static volatile bool wifi_scan_running = false;
static char wifi_status_message[96] = "Aucun reseau configure.";

static void http_send_wifi(int client);

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

static void wifi_scan_done_cb(void *arg, STATUS status)
{
    wifi_scan_count = 0;
    wifi_scan_running = false;

    if (status != OK || arg == NULL) {
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "Scan WiFi echoue.");
        WEB_LOG("wifi scan failed status=%d", status);
        return;
    }

    struct bss_info *bss = (struct bss_info *)arg;
    while (bss != NULL && wifi_scan_count < WIFI_SCAN_MAX_RESULTS) {
        uint8_t len = bss->ssid_len;
        if (len > 32) {
            len = 32;
        }

        if (len > 0) {
            memset(&wifi_scan_results[wifi_scan_count], 0, sizeof(wifi_scan_results[0]));
            memcpy(wifi_scan_results[wifi_scan_count].ssid, bss->ssid, len);
            wifi_scan_results[wifi_scan_count].ssid[len] = '\0';
            wifi_scan_results[wifi_scan_count].rssi = bss->rssi;
            wifi_scan_results[wifi_scan_count].authmode = bss->authmode;
            WEB_LOG("scan result ssid=%s rssi=%d auth=%d",
                    wifi_scan_results[wifi_scan_count].ssid,
                    wifi_scan_results[wifi_scan_count].rssi,
                    wifi_scan_results[wifi_scan_count].authmode);
            wifi_scan_count++;
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

    wifi_set_opmode_current(STATIONAP_MODE);

    struct scan_config config;
    memset(&config, 0, sizeof(config));
    config.show_hidden = 0;

    snprintf(wifi_status_message, sizeof(wifi_status_message),
             "Scan WiFi en cours...");

    wifi_scan_running = wifi_station_scan(&config, wifi_scan_done_cb);
    if (!wifi_scan_running) {
        snprintf(wifi_status_message, sizeof(wifi_status_message),
                 "Impossible de demarrer le scan WiFi.");
        WEB_LOG("wifi scan start failed");
    } else {
        WEB_LOG("wifi scan started");
    }
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

    while (remaining > 0) {
        int sent = send(client, p, remaining, 0);
        if (sent <= 0) {
            WEB_LOG("http send failed remaining=%d", remaining);
            return;
        }

        p += sent;
        remaining -= sent;
    }
}

static void http_send_header(int client, const char *status, const char *content_type)
{
    char header[160];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %s\r\n"
             "Content-Type: %s\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             status, content_type);
    http_send(client, header);
}

static void html_escape_send(int client, const char *s)
{
    while (*s != '\0') {
        switch (*s) {
        case '&':
            http_send(client, "&amp;");
            break;
        case '<':
            http_send(client, "&lt;");
            break;
        case '>':
            http_send(client, "&gt;");
            break;
        case '"':
            http_send(client, "&quot;");
            break;
        default: {
            char c[2] = { *s, '\0' };
            http_send(client, c);
            break;
        }
        }
        s++;
    }
}

static void http_send_page_start(int client, const char *title)
{
    http_send_header(client, "200 OK", "text/html; charset=utf-8");
    http_send(client,
              "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>");
    html_escape_send(client, title);
    http_send(client,
              "</title><style>"
              "body{font-family:Arial,sans-serif;margin:24px;max-width:720px;background:#f6f7f9;color:#15171a}"
              "main{background:#fff;border:1px solid #d8dde3;border-radius:8px;padding:18px}"
              "a,button{display:inline-block;margin:6px 6px 6px 0;padding:10px 14px;border-radius:6px;border:0;background:#1f6feb;color:#fff;text-decoration:none;font-size:16px}"
              ".off{background:#b42318}.muted{color:#5b6470}.field{margin:12px 0}input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px}"
              "</style></head><body><main>");
    http_send(client, "<h1>");
    html_escape_send(client, title);
    http_send(client, "</h1>");
}

static void http_send_page_end(int client)
{
    http_send(client, "</main></body></html>");
}

static void http_send_home(int client)
{
    char buf[256];

    http_send_page_start(client, "Laser Cat Toy");
    snprintf(buf, sizeof(buf),
             "<p>Jouet: <strong>%s</strong></p>"
             "<p>WiFi station: <strong>%s</strong></p>"
             "<p class='muted'>%s</p>",
             game_is_enabled() ? "ON" : "OFF",
             station_status_text(),
             wifi_status_message);
    http_send(client, buf);
    http_send(client, "<p><a href='/on'>ON</a><a class='off' href='/off'>OFF</a><a href='/wifi'>Configurer WiFi</a></p>");
    http_send_page_end(client);
}

static void http_send_captive(int client)
{
    WEB_LOG("captive page served");
    http_send_wifi(client);
}

static void http_send_wifi(int client)
{
    char buf[256];

    if (wifi_scan_count == 0 && !wifi_scan_running) {
        WEB_LOG("wifi page opened without scan results, auto scan");
        wifi_start_scan();
    }

    http_send_page_start(client, "Configuration WiFi");
    if (wifi_scan_running) {
        http_send(client, "<script>setTimeout(function(){location.href='/wifi'},2000)</script>");
    }
    snprintf(buf, sizeof(buf),
             "<p>AP de configuration: <strong>%s</strong></p>"
             "<p>Station: <strong>%s</strong></p>"
             "<p class='muted'>%s</p>",
             CONFIG_AP_SSID,
             station_status_text(),
             wifi_status_message);
    http_send(client, buf);
    http_send(client, "<p><a href='/scan'>Rescanner</a><a href='/'>Retour</a></p>");
    http_send(client, "<form action='/connect' method='get'>");
    http_send(client, "<div class='field'><label>Reseau detecte</label><select name='ssid'>");

    if (wifi_scan_running) {
        http_send(client, "<option value=''>Scan en cours...</option>");
    } else if (wifi_scan_count == 0) {
        http_send(client, "<option value=''>Aucun reseau scanne</option>");
    } else {
        for (uint8_t i = 0; i < wifi_scan_count; i++) {
            http_send(client, "<option value=\"");
            html_escape_send(client, wifi_scan_results[i].ssid);
            http_send(client, "\">");
            html_escape_send(client, wifi_scan_results[i].ssid);
            snprintf(buf, sizeof(buf), " (%d dBm)%s</option>",
                     wifi_scan_results[i].rssi,
                     wifi_scan_results[i].authmode == AUTH_OPEN ? " ouvert" : "");
            http_send(client, buf);
        }
    }

    http_send(client, "</select></div>");
    http_send(client, "<div class='field'><label>Ou SSID manuel</label><input name='manual_ssid'></div>");
    http_send(client, "<div class='field'><label>Mot de passe</label><input name='pass' type='password'></div>");
    http_send(client, "<button type='submit'>Connecter</button></form>");
    http_send_page_end(client);
}

static void http_redirect(int client, const char *location)
{
    char header[160];
    snprintf(header, sizeof(header),
             "HTTP/1.1 303 See Other\r\n"
             "Location: %s\r\n"
             "Connection: close\r\n"
             "\r\n",
             location);
    http_send(client, header);
}

static void http_handle_request(int client, char *request)
{
    char path[384];
    char *start = strchr(request, ' ');
    char *end;

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
        wifi_start_scan();
        http_redirect(client, "/wifi");
    } else if (strncmp(path, "/connect?", 9) == 0) {
        char ssid[33];
        char manual_ssid[33];
        char password[65];
        const char *query = path + 9;

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
    } else if (strcmp(path, "/wifi") == 0) {
        WEB_LOG("wifi page requested status=%s count=%d running=%d",
                station_status_text(), wifi_scan_count, wifi_scan_running);
        http_send_wifi(client);
    } else if (strcmp(path, "/") == 0) {
        WEB_LOG("home page requested toy=%d station=%s",
                game_is_enabled(), station_status_text());
        http_send_home(client);
    } else if (strcmp(path, "/generate_204") == 0 ||
               strcmp(path, "/gen_204") == 0 ||
               strcmp(path, "/hotspot-detect.html") == 0 ||
               strcmp(path, "/library/test/success.html") == 0 ||
               strcmp(path, "/ncsi.txt") == 0 ||
               strcmp(path, "/connecttest.txt") == 0 ||
               strcmp(path, "/redirect") == 0) {
        WEB_LOG("captive probe path=%s", path);
        http_send_captive(client);
    } else {
        WEB_LOG("unknown path served as captive path=%s", path);
        http_send_captive(client);
    }
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
        os_printf("dns socket failed\n");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0;
    addr.sin_port = htons(DNS_PORT);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        os_printf("dns bind failed\n");
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
        os_printf("http socket failed\n");
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
        os_printf("http bind/listen failed\n");
        closesocket(server);
        vTaskDelete(NULL);
        return;
    }

    WEB_LOG("http server ready url=http://192.168.4.1/");

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
            WEB_LOG("http request method=%s path=%s", method, path);
            http_handle_request(client, request);
        }

        closesocket(client);
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
            last_status = status;
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
    WEB_LOG("config ap ready ssid=%s ip=%d.%d.%d.%d http_port=%d dns_port=%d",
            CONFIG_AP_SSID,
            CONFIG_AP_IP_A, CONFIG_AP_IP_B, CONFIG_AP_IP_C, CONFIG_AP_IP_D,
            HTTP_PORT, DNS_PORT);
}
