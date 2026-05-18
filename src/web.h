#ifndef LASER_CAT_TOY_WEB_H
#define LASER_CAT_TOY_WEB_H

void web_portal_init(void);
void web_http_server_task(void *arg);
void web_dns_server_task(void *arg);
void web_wifi_status_task(void *arg);
void web_captive_dns_start(void);
void web_captive_dns_stop(void);

#endif
