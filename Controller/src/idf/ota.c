#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// GET /ota -> upload page; POST /ota -> raw firmware.bin. The image is written
// to the inactive slot, validated, then selected and booted. Rollback is armed
// by CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE; ota_mark_valid() confirms the
// running image from app_main().

static const char *TAG = "ota";

#define OTA_CHUNK 4096

static esp_timer_handle_t s_restart_timer;

static const char PAGE_HEAD[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Firmware</title><style>"
"body{background:#1b0505;color:#f2dada;font:16px system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;line-height:1.5}"
"h1{color:#ff8a8a;margin:.2rem 0 1rem}code,pre{font-family:ui-monospace,monospace}"
"input,button{font:inherit;background:#3a0d0d;color:#f2dada;border:1px solid #7a1f1f;padding:.4rem}"
"button{background:#7a1f1f;cursor:pointer}button:hover{background:#9b2626}"
"progress{width:100%;height:1rem;accent-color:#c0392b}pre{white-space:pre-wrap}"
"</style><h1>Firmware update</h1><p>";

static const char PAGE_BODY[] =
"</p><p><input id=f type=file accept=.bin><button onclick=u()>Upload &amp; reboot</button></p>"
"<progress id=p max=100 value=0></progress><pre id=s></pre>"
"<p><code>curl --data-binary @firmware.bin http://&lt;host&gt;/ota</code></p>"
"<script>function u(){var f=document.getElementById('f').files[0],s=document.getElementById('s'),p=document.getElementById('p');"
"if(!f){s.textContent='Choose a .bin';return}"
"var x=new XMLHttpRequest();x.open('POST','/ota');"
"x.upload.onprogress=function(e){p.value=e.loaded*100/e.total;s.textContent=e.loaded+'/'+e.total};"
"x.onload=function(){s.textContent=x.status+' '+x.responseText};x.send(f)}</script>";

static const char *const MONTHS[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static void iso_time(const esp_app_desc_t *app, char *out, size_t cap)
{
    char mon[4] = {0};
    int day = 0;
    int year = 0;

    if (app != NULL && sscanf(app->date, "%3s %d %d", mon, &day, &year) == 3) {
        for (int i = 0; i < 12; i++) {
            if (strcmp(mon, MONTHS[i]) == 0) {
                snprintf(out, cap, "%04d-%02d-%02dT%s", year, i + 1, day,
                         app->time);
                return;
            }
        }
    }
    snprintf(out, cap, "unknown");
}

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void restart_later(void)
{
    esp_timer_stop(s_restart_timer);
    if (esp_timer_start_once(s_restart_timer, 500 * 1000) != ESP_OK) {
        esp_restart();
    }
}

void ota_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "app marked valid");
    } else if (err != ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        ESP_LOGW(TAG, "mark valid: %s", esp_err_to_name(err));
    }
}

static esp_err_t page_get(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    char iso[32];
    char line[192];

    iso_time(app, iso, sizeof(iso));
    int len = snprintf(line, sizeof(line),
                       "Running <b>%s</b> &middot; %s &middot; %s",
                       part != NULL ? part->label : "?",
                       iso, app != NULL ? app->version : "?");
    if (len <= 0 || (size_t)len >= sizeof(line)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "page too large");
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = httpd_resp_send_chunk(req, PAGE_HEAD,
                                          HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, line, len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, PAGE_BODY,
                                    HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t image_post(httpd_req_t *req)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no OTA partition");
    }
    if (req->content_len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "empty body");
    }
    if ((size_t)req->content_len > update->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "image too large");
    }

    ESP_LOGI(TAG, "%u bytes -> %s @0x%lx", (unsigned)req->content_len,
             update->label, (unsigned long)update->address);

    char *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "out of memory");
    }

    esp_ota_handle_t handle = 0;
    int64_t t_begin = esp_timer_get_time();
    esp_err_t err = esp_ota_begin(update, (size_t)req->content_len, &handle);
    int64_t begin_ms = (esp_timer_get_time() - t_begin) / 1000;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "begin: %s", esp_err_to_name(err));
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "OTA begin failed");
    }

    size_t remaining = (size_t)req->content_len;
    int64_t t_write = esp_timer_get_time();
    while (remaining > 0) {
        int want = remaining < OTA_CHUNK ? (int)remaining : OTA_CHUNK;
        int got = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            ESP_LOGE(TAG, "receive failed");
            esp_ota_abort(handle);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "receive failed");
        }
        err = esp_ota_write(handle, buf, (size_t)got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                           "invalid image");
            }
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "flash write failed");
        }
        remaining -= (size_t)got;
    }
    free(buf);

    int64_t write_ms = (esp_timer_get_time() - t_write) / 1000;
    int64_t verify_start = esp_timer_get_time();
    err = esp_ota_end(handle);
    int64_t verify_ms = (esp_timer_get_time() - verify_start) / 1000;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "end (%lld ms): %s", (long long)verify_ms,
                 esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid image");
    }
    ESP_LOGI(TAG, "begin %lld ms, write %lld ms, verify %lld ms",
             (long long)begin_ms, (long long)write_ms, (long long)verify_ms);
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "could not select image");
    }

    ESP_LOGI(TAG, "written to %s", update->label);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t send_err = httpd_resp_sendstr(req, "OK\n");
    restart_later();
    return send_err;
}

esp_err_t ota_http_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_timer_create_args_t args = {
        .callback = restart_cb,
        .name = "ota_restart",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_restart_timer), TAG,
                        "timer");

    const httpd_uri_t get = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = page_get,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get), TAG,
                        "GET /ota");

    const httpd_uri_t post = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = image_post,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post), TAG,
                        "POST /ota");
    return ESP_OK;
}