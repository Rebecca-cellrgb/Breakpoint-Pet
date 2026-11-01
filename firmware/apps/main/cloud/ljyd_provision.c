#include "ljyd_provision.h"
#include "ljyd_cloud.h"
#include "bsp_camera.h"

#include "qm.h"
#include "qm_iot_dynreg.h"
#include "qm_kv.h"
#include "qm_provision.h"
#include "qm_wifi.h"
#include "qm_wifi_dev.h"
#include "qm_work.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "provision"

#define PROVISION_KV_BIND      "cloud/bind"
#define PROVISION_KV_ENV       "cloud/env"
/* 蓝牙断连后再等 500ms 才 qm_ble_end。1s 时 Wi-Fi 和协议栈抢内部 RAM，静态 RX 缓冲分配失败。 */
#define PROVISION_WIFI_DELAY_MS 2000
#define SETUP_AP_SSID          "DuanDianCam-Setup"
#define SETUP_AP_PASSWORD      "camera1234"
#define SETUP_HTTP_PORT        80
#define SETUP_REQUEST_MAX      1024

static qm_provision_handle_t provision_handle;
static uint8_t wifi_event_registered;
static qm_work_t wifi_delay_work;
static qm_work_t wifi_fallback_work;
static qm_task_t setup_task;
static char pending_ssid[CONFIG_QM_WIFI_SSID_MAX_LEN + 1];
static char pending_password[CONFIG_QM_WIFI_PASSWD_MAX_LEN + 1];
static unsigned wifi_disconnect_count;
static uint8_t wifi_sta_connected;
static uint8_t setup_ap_started;
static uint8_t setup_fallback_scheduled;

static int provision_apply_wifi(const char *ssid, const char *password);
static void setup_http_task(void *arg);

static const char setup_page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>桌宠联网</title><h2>连接桌宠到 Wi-Fi</h2>"
    "<form method=post action=/save>"
    "<p>Wi-Fi 名称<br><input name=ssid maxlength=32 required></p>"
    "<p>Wi-Fi 密码<br><input name=password type=password maxlength=64></p>"
    "<button>连接</button></form><p>密码只发送给本机主板。</p>";

static int setup_send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        int sent = send(fd, data, len, 0);
        if (sent <= 0) return -1;
        data += sent;
        len -= (size_t)sent;
    }
    return 0;
}

static int setup_send_response(int fd, const char *body)
{
    char header[128];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                              "Content-Length: %u\r\nConnection: close\r\n\r\n",
                              (unsigned)strlen(body));
    if (header_len <= 0 || setup_send_all(fd, header, (size_t)header_len) != 0) return -1;
    return setup_send_all(fd, body, strlen(body));
}

static int setup_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void setup_decode_field(const char *body, const char *name,
                               char *dst, size_t dst_size)
{
    char key[24];
    const char *field;
    size_t key_len = (size_t)snprintf(key, sizeof(key), "%s=", name);
    size_t out = 0;

    if (key_len >= sizeof(key)) return;
    field = strstr(body, key);
    if (field == NULL || (field != body && field[-1] != '&')) return;
    field += key_len;
    while (*field != '\0' && *field != '&' && out + 1 < dst_size) {
        if (*field == '+') {
            dst[out++] = ' ';
            field++;
        } else if (*field == '%' && field[1] != '\0' && field[2] != '\0') {
            int hi = setup_hex_value(field[1]);
            int lo = setup_hex_value(field[2]);
            if (hi < 0 || lo < 0) return;
            dst[out++] = (char)((hi << 4) | lo);
            field += 3;
        } else {
            dst[out++] = *field++;
        }
    }
    dst[out] = '\0';
}

static void setup_apply_wifi(void *arg)
{
    int ret;
    (void)arg;

    (void)qm_wifi_stop();
    setup_ap_started = 0;
    ret = qm_wifi_set_mode(QM_WIFI_MODE_STA);
    if (ret == QM_EOK) ret = provision_apply_wifi(pending_ssid, pending_password);
    if (ret == QM_EOK) ret = qm_wifi_start();
    QM_LOGI(LOG_TAG, "setup Wi-Fi start ssid=%s ret=%d", pending_ssid, ret);
}

static int provision_enable_setup_ap(bool stop_wifi)
{
    qm_wifi_config_t config = {0};
    int ret;

    if (setup_ap_started) return QM_EOK;
    setup_ap_started = 1;
    if (stop_wifi) (void)qm_wifi_stop();

    memcpy(config.ap.ssid, SETUP_AP_SSID, sizeof(SETUP_AP_SSID) - 1);
    config.ap.ssid_len = (uint8_t)(sizeof(SETUP_AP_SSID) - 1);
    memcpy(config.ap.password, SETUP_AP_PASSWORD, sizeof(SETUP_AP_PASSWORD) - 1);
    config.ap.password_len = (uint8_t)(sizeof(SETUP_AP_PASSWORD) - 1);
    config.ap.max_connection = 2;
    config.ap.authmode = QM_WIFI_AUTH_WPA2_PSK;

    ret = qm_wifi_set_mode(QM_WIFI_MODE_AP);
    if (ret == QM_EOK) ret = qm_wifi_set_config(QM_WIFI_IF_AP, &config);
    if (ret == QM_EOK) ret = qm_wifi_start();
    if (ret != QM_EOK) {
        setup_ap_started = 0;
        return ret;
    }

    QM_LOGI(LOG_TAG, "setup Wi-Fi: SSID=%s password=%s", SETUP_AP_SSID, SETUP_AP_PASSWORD);
    return qm_task_new(&setup_task, "wifi_setup", setup_http_task, NULL, 6144, 5);
}

static void setup_fallback_work(void *arg)
{
    (void)arg;
    setup_fallback_scheduled = 0;
    if (wifi_disconnect_count >= 3 && !wifi_sta_connected && !setup_ap_started) {
        QM_LOGW(LOG_TAG, "saved Wi-Fi unavailable, opening local setup page");
        int ret = provision_enable_setup_ap(true);
        if (ret != QM_EOK) QM_LOGE(LOG_TAG, "setup AP failed ret=%d", ret);
    }
}

static void setup_schedule_fallback(void)
{
    if (setup_ap_started || setup_fallback_scheduled) return;
    setup_fallback_scheduled = 1;
    (void)qm_post_delayed_action(&wifi_fallback_work, setup_fallback_work, NULL, 1000);
}

static void setup_http_task(void *arg)
{
    int listener = -1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(SETUP_HTTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    (void)arg;

    for (int attempt = 0; attempt < 10; attempt++) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listener >= 0 && bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 &&
            listen(listener, 1) == 0) {
            break;
        }
        int err = errno;
        if (listener >= 0) close(listener);
        listener = -1;
        QM_LOGW(LOG_TAG, "setup page socket retry=%d errno=%d", attempt + 1, err);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (listener < 0) {
        QM_LOGE(LOG_TAG, "setup page socket unavailable");
        vTaskDelete(NULL);
        return;
    }

    QM_LOGI(LOG_TAG, "setup page ready at 192.168.4.1");
    for (;;) {
        int client = accept(listener, NULL, NULL);
        char request[SETUP_REQUEST_MAX + 1] = {0};
        int used = 0;
        char *body;
        if (client < 0) continue;

        while (used < SETUP_REQUEST_MAX) {
            int got = recv(client, request + used, SETUP_REQUEST_MAX - used, 0);
            if (got <= 0) break;
            used += got;
            request[used] = '\0';
            if (strstr(request, "\r\n\r\n") != NULL) break;
        }

        if (strncmp(request, "GET /", 5) == 0) {
            (void)setup_send_response(client, setup_page);
        } else if (strncmp(request, "POST /save", 10) == 0) {
            body = strstr(request, "\r\n\r\n");
            if (body != NULL) {
                char *length_header = strstr(request, "Content-Length:");
                int expected = length_header ? atoi(length_header + 15) : 0;
                body += 4;
                while (expected > used - (int)(body - request) && used < SETUP_REQUEST_MAX) {
                    int got = recv(client, request + used, SETUP_REQUEST_MAX - used, 0);
                    if (got <= 0) break;
                    used += got;
                    request[used] = '\0';
                }
                setup_decode_field(body, "ssid", pending_ssid, sizeof(pending_ssid));
                setup_decode_field(body, "password", pending_password, sizeof(pending_password));
            }
            if (pending_ssid[0] != '\0') {
                static const char saved[] = "<meta charset=utf-8><h2>已收到，主板正在连接 Wi-Fi。请稍等。</h2>";
                (void)setup_send_response(client, saved);
                close(client);
                close(listener);
                vTaskDelay(pdMS_TO_TICKS(1000));
                (void)qm_post_delayed_action(&wifi_delay_work, setup_apply_wifi,
                                             NULL, PROVISION_WIFI_DELAY_MS);
                vTaskDelete(NULL);
                return;
            }
            (void)setup_send_response(client, "<meta charset=utf-8><h2>Wi-Fi 名称不能为空，请返回重试。</h2>");
        }
        close(client);
    }
}

static int provision_start_setup_ap(void)
{
    int ret = qm_wifi_init();
    if (ret != QM_EOK) return ret;
    return provision_enable_setup_ap(false);
}

static void provision_env_load(void)
{
    uint8_t env = 0;
    int len = (int)sizeof(env);

    if (qm_kv_get(PROVISION_KV_ENV, &env, &len) != QM_EOK || len != (int)sizeof(env)) {
        return;
    }
    if (env > (uint8_t)QM_IOT_DYNREG_ENV_PROD) {
        return;
    }
    (void)qm_iot_dynreg_env_set((qm_iot_dynreg_env_t)env);
    QM_LOGI(LOG_TAG, "dynreg env=%u", (unsigned)env);
}

static void provision_env_save(uint8_t env)
{
    if (env > (uint8_t)QM_IOT_DYNREG_ENV_PROD) {
        return;
    }
    (void)qm_iot_dynreg_env_set((qm_iot_dynreg_env_t)env);
    (void)qm_kv_set(PROVISION_KV_ENV, &env, (int)sizeof(env), 1);
    QM_LOGI(LOG_TAG, "dynreg env save=%u", (unsigned)env);
}

static void provision_set_bound(uint8_t bound)
{
    (void)qm_kv_set(PROVISION_KV_BIND, &bound, (int)sizeof(bound), 1);
}

void ljyd_provision_clear_and_reboot(void)
{
    uint8_t bound = 0;

    (void)qm_kv_set(PROVISION_KV_BIND, &bound, (int)sizeof(bound), 1);
    (void)qm_kv_del(PROVISION_KV_ENV);
    (void)qm_kv_del("wifi_cfg");
    qm_msleep(50);
    qm_reboot();
}

static int provision_apply_wifi(const char *ssid, const char *password)
{
    qm_wifi_config_t config = {0};
    uint32_t ssid_len;
    uint32_t password_len;

    if (ssid == NULL || ssid[0] == '\0') {
        return -QM_EINVAL;
    }
    ssid_len = (uint32_t)strlen(ssid);
    password_len = (password != NULL) ? (uint32_t)strlen(password) : 0;
    if (ssid_len > CONFIG_QM_WIFI_SSID_MAX_LEN || password_len > CONFIG_QM_WIFI_PASSWD_MAX_LEN) {
        return -QM_EINVAL;
    }
    memcpy(config.sta.ssid, ssid, ssid_len);
    config.sta.ssid_len = (uint8_t)ssid_len;
    if (password_len > 0) {
        memcpy(config.sta.password, password, password_len);
        config.sta.password_len = (uint8_t)password_len;
    }
    return qm_wifi_set_config(QM_WIFI_IF_STA, &config);
}

static int provision_has_saved_wifi(void)
{
    qm_wifi_cfg_t cfg;
    int len = (int)sizeof(cfg);

    memset(&cfg, 0, sizeof(cfg));
    if (qm_kv_get("wifi_cfg", &cfg, &len) != QM_EOK || len != (int)sizeof(cfg)) {
        return 0;
    }
    return cfg.config.sta.ssid_len > 0 && cfg.config.sta.ssid[0] != '\0';
}

static int provision_start_wifi(void)
{
    int ret = qm_wifi_init();
    if (ret != QM_EOK) {
        return ret;
    }
    ret = qm_wifi_set_mode(QM_WIFI_MODE_STA);
    if (ret != QM_EOK) {
        return ret;
    }
    return qm_wifi_start();
}

static void provision_wifi_connect_later(void *arg)
{
    int ret;

    (void)arg;
    ret = qm_wifi_init();
    if (ret != QM_EOK) {
        QM_LOGE(LOG_TAG, "wifi init ret=%d", ret);
        return;
    }
    ret = qm_wifi_set_mode(QM_WIFI_MODE_STA);
    if (ret != QM_EOK) {
        QM_LOGE(LOG_TAG, "wifi set mode ret=%d", ret);
        return;
    }
    ret = provision_apply_wifi(pending_ssid, pending_password);
    if (ret != QM_EOK) {
        QM_LOGE(LOG_TAG, "save provision wifi failed ret=%d", ret);
        return;
    }
    ret = qm_wifi_start();
    QM_LOGI(LOG_TAG, "wifi start ssid=%s ret=%d", pending_ssid, ret);
}

static void provision_recv(qm_provision_handle_t handle,
                           const qm_provision_recv_t *recv,
                           void *userdata)
{
    (void)userdata;
    if (recv == NULL || recv->ssid[0] == '\0') {
        return;
    }
    QM_LOGI(LOG_TAG, "provision ssid=%s", recv->ssid);
    if (recv->env_type_valid) {
        provision_env_save(recv->env_type);
    }
    memset(pending_ssid, 0, sizeof(pending_ssid));
    memset(pending_password, 0, sizeof(pending_password));
    strncpy(pending_ssid, recv->ssid, sizeof(pending_ssid) - 1);
    if (recv->password[0] != '\0') {
        strncpy(pending_password, recv->password, sizeof(pending_password) - 1);
    }
    provision_set_bound(1);
    if (handle != NULL) {
        qm_provision_destroy(handle);
        if (provision_handle == handle) {
            provision_handle = NULL;
        }
        QM_LOGI(LOG_TAG, "ble stopped");
    }
    (void)qm_post_delayed_action(&wifi_delay_work, provision_wifi_connect_later,
                                 NULL, PROVISION_WIFI_DELAY_MS);
}

static int provision_start_ble(void)
{
    qm_provision_config_t config = {
        .product_id = CONFIG_XJIANG_CLOUD_PID,
        .product_secret = CONFIG_XJIANG_CLOUD_SECERT,
        .adv_cid = CONFIG_QM_IOT_PROV_BLE_ADV_CID,
        .recv_handler = provision_recv,
        .userdata = NULL,
    };
    int ret;

    if (provision_handle == NULL) {
        provision_handle = qm_provision_create(&config);
        if (provision_handle == NULL) {
            return -QM_ENOMEM;
        }
    }
    ret = qm_provision_start(provision_handle);
    QM_LOGI(LOG_TAG, "ble provision start ret=%d", ret);
    return ret;
}

static void provision_wifi_event_handler(void *arg, qm_event_base_t event_base,
                                         int32_t event_id, void *event_data,
                                         size_t event_data_size)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    (void)event_data_size;

    switch ((qm_wifi_event_t)event_id) {
        case QM_WIFI_EVENT_STA_START:
            QM_LOGI(LOG_TAG, "wifi sta start");
            (void)qm_wifi_connect();
            break;
        case QM_WIFI_EVENT_STA_GOT_IP:
            QM_LOGI(LOG_TAG, "wifi got ip");
            wifi_sta_connected = 1;
            setup_fallback_scheduled = 0;
            (void)qm_cancel_delayed_action(&wifi_fallback_work);
            wifi_disconnect_count = 0;
#if defined(CONFIG_APP_CAMERA_TEST) && CONFIG_APP_CAMERA_TEST
            if (bsp_camera_stream_start() != ESP_OK) {
                QM_LOGE(LOG_TAG, "camera stream start failed after Wi-Fi connected");
            }
#endif
            ljyd_cloud_on_network_up();
            break;
        case QM_WIFI_EVENT_STA_CONNECTED:
            QM_LOGI(LOG_TAG, "wifi connected to access point");
            wifi_sta_connected = 1;
            setup_fallback_scheduled = 0;
            (void)qm_cancel_delayed_action(&wifi_fallback_work);
            break;
        case QM_WIFI_EVENT_STA_DISCONNECTED:
            QM_LOGW(LOG_TAG, "wifi disconnected");
            wifi_sta_connected = 0;
            ljyd_cloud_on_network_down();
            if (++wifi_disconnect_count >= 3) setup_schedule_fallback();
            (void)qm_wifi_connect();
            break;
        default:
            break;
    }
}

int ljyd_provision_init(void)
{
    int ret;

    if (!wifi_event_registered) {
        ret = qm_event_handler_register(QM_EVENT_WIFI, QM_EVENT_ANY_ID,
                                        provision_wifi_event_handler, NULL);
        if (ret != QM_EOK) {
            QM_LOGE(LOG_TAG, "wifi event register failed ret=%d", ret);
            return ret;
        }
        wifi_event_registered = 1;
    }

    provision_env_load();

    if (provision_has_saved_wifi()) {
        QM_LOGI(LOG_TAG, "saved wifi, skip ble");
        return provision_start_wifi();
    }

    QM_LOGI(LOG_TAG, "no saved wifi, start local setup page");
    ret = provision_start_setup_ap();
    if (ret != QM_EOK) {
        QM_LOGE(LOG_TAG, "local setup page failed ret=%d, falling back to BLE", ret);
        return provision_start_ble();
    }
    return ret;
}
