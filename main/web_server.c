/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <esp_http_server.h>
#include <esp_log.h>
#include <wifi.h>
#include <cJSON.h>
#include <sys/param.h>
#include <esp_vfs.h>
#include <esp_spiffs.h>
#include <lwip/apps/mdns.h>
#include <config.h>
#include <log.h>
#include <core_dump.h>
#include <util.h>
#include <lwip/inet.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_flash.h>
#include <esp_rom_md5.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <stream_stats.h>
#include <esp32/rom/crc.h>
#include <lwip/sockets.h>
#include <esp_timer.h>
#include <esp_system.h>
#include "driver/temp_sensor.h"
#include "web_server.h"
#include "uart.h"

// Max length a file path can have on storage
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
#define FILE_HASH_SUFFIX ".crc"

#define WWW_PARTITION_PATH "/www"
#define WWW_PARTITION_LABEL "www"
#define BUFFER_SIZE 2048

static const char *TAG = "WEB";

static char *buffer;

enum auth_method {
    AUTH_METHOD_OPEN = 0,
    AUTH_METHOD_HOTSPOT = 1,
    AUTH_METHOD_BASIC = 2
};

static char *basic_authentication;
static enum auth_method auth_method;

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

static esp_err_t www_spiffs_init() {
    ESP_LOGD(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
            .base_path = WWW_PARTITION_PATH,
            .partition_label = WWW_PARTITION_LABEL,
            .max_files = 10,
            .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(WWW_PARTITION_LABEL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

// Set HTTP response content type according to file extension
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".js")) {
        return httpd_resp_set_type(req, "application/javascript");
    } else if (IS_FILE_EXT(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        // Full path string won't fit into destination buffer
        return NULL;
    }

    // Construct full path (base + path)
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    // Return pointer to path, skipping the base
    return dest + base_pathlen;
}

static esp_err_t json_response(httpd_req_t *req, cJSON *root) {
    // Set mime type
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) return err;

    // Convert to string
    bool success = cJSON_PrintPreallocated(root, buffer, BUFFER_SIZE, false);
    cJSON_Delete(root);
    if (!success) {
        ESP_LOGE(TAG, "Not enough space in buffer to output JSON");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough space in buffer to output JSON");
        return ESP_FAIL;
    }

    // Send as response
    err = httpd_resp_send(req, buffer, strlen(buffer));
    if (err != ESP_OK) return err;

    return ESP_OK;
}

static esp_err_t basic_auth(httpd_req_t *req) {
    int authorization_length = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (authorization_length == 0) goto _auth_required;

    char *authorization_header = malloc(authorization_length);
    httpd_req_get_hdr_value_str(req, "Authorization", authorization_header, authorization_length);

    bool authenticated = strcasecmp(basic_authentication, authorization_header) == 0;
    free(authorization_header);

    if (authenticated) return ESP_OK;

    _auth_required:
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 XBee Config\"");
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Incorrect or no password provided";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t hotspot_auth(httpd_req_t *req) {
    int sock = httpd_req_to_sockfd(req);

    struct sockaddr_in6 client_addr;
    socklen_t socklen = sizeof(client_addr);
    getpeername(sock, (struct sockaddr *)&client_addr, &socklen);

    // TODO: Correctly read IPv4?
    // ERROR_ACTION(TAG, client_addr.sin6_family != AF_INET, goto _auth_error, "IPv6 connections not supported, IP family %d", client_addr.sin6_family);

    wifi_sta_list_t *ap_sta_list = wifi_ap_sta_list();
    wifi_sta_mac_ip_list_t esp_netif_ap_sta_list;
    esp_wifi_ap_get_sta_list_with_ip(ap_sta_list, &esp_netif_ap_sta_list);

    // TODO: Correctly read IPv4?
    for (int i = 0; i < esp_netif_ap_sta_list.num; i++) {
        if (esp_netif_ap_sta_list.sta[i].ip.addr == client_addr.sin6_addr.un.u32_addr[3]) return ESP_OK;
    }

    //_auth_error:
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Configured to only accept connections from hotspot devices";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t check_auth(httpd_req_t *req) {
    if (auth_method == AUTH_METHOD_HOTSPOT) return hotspot_auth(req);
    if (auth_method == AUTH_METHOD_BASIC) return basic_auth(req);
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    httpd_resp_set_type(req, "text/plain");

    size_t length;
    void *log_data = log_receive(&length, 1);
    if (log_data == NULL) {
        httpd_resp_sendstr(req, "");

        return ESP_OK;
    }

    httpd_resp_send(req, log_data, length);

    log_return(log_data);

    return ESP_OK;
}

static esp_err_t core_dump_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    size_t core_dump_size = core_dump_available();
    if (core_dump_size == 0) {
        httpd_resp_sendstr(req, "No core dump available");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");

    const esp_app_desc_t *app_desc = esp_app_get_description();

    char elf_sha256[7];
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));

    time_t t = time(NULL);
    char date[20] = "\0";
    if (t > 315360000l) strftime(date, sizeof(date), "_%F_%T", localtime(&t));

    char content_disposition[128];
    snprintf(content_disposition, sizeof(content_disposition),
            "attachment; filename=\"esp32_xbee_%s_core_dump_%s%s.bin\"", app_desc->version, elf_sha256, date);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);

    for (int offset = 0; offset < core_dump_size; offset += BUFFER_SIZE) {
        size_t read = core_dump_size - offset;
        if (read > BUFFER_SIZE) read = BUFFER_SIZE;

        core_dump_read(offset, buffer, read);
        httpd_resp_send_chunk(req, buffer, read);
    }

    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t ota_page_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    char html[2048];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>Firmware Update</title>"
        "<style>body{font-family:sans-serif;max-width:500px;margin:40px auto;padding:0 20px}"
        "button{padding:8px 16px;margin-top:10px;cursor:pointer}"
        "#status{margin-top:16px;font-weight:bold}"
        "#bar{width:100%%;height:20px;background:#eee;border-radius:4px;margin-top:8px;display:none}"
        "#fill{height:100%%;width:0;background:#4caf50;border-radius:4px;transition:width 0.2s}"
        "</style></head><body>"
        "<h2>Firmware Update</h2>"
        "<p>Running: <b>%s</b> on partition <b>%s</b></p>"
        "<p>Update target: <b>%s</b></p>"
        "<input type='file' id='f' accept='.bin'><br>"
        "<button onclick='upload()'>Flash Firmware</button>"
        "<div id='bar'><div id='fill'></div></div>"
        "<div id='status'></div>"
        "<script>"
        "function upload(){"
        "var f=document.getElementById('f').files[0];"
        "if(!f){alert('Select a .bin file first');return;}"
        "var s=document.getElementById('status');"
        "var bar=document.getElementById('bar');"
        "var fill=document.getElementById('fill');"
        "bar.style.display='block';"
        "s.textContent='Uploading '+f.name+' ('+f.size+' bytes)...';"
        "var xhr=new XMLHttpRequest();"
        "xhr.upload.onprogress=function(e){"
        "if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);"
        "fill.style.width=p+'%%';s.textContent='Uploading... '+p+'%%';}"
        "};"
        "xhr.onload=function(){"
        "if(xhr.status==200){s.textContent='Done! Rebooting... reconnect in ~10 seconds.';fill.style.background='#4caf50';}"
        "else{s.textContent='Error: '+xhr.responseText;fill.style.background='#f44336';}"
        "};"
        "xhr.onerror=function(){s.textContent='Upload failed (connection lost)';};"
        "xhr.open('POST','/ota');"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.send(f);"
        "}"
        "</script></body></html>",
        app_desc->version,
        running ? running->label : "unknown",
        next ? next->label : "none - repartition required"
    );

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

/* ---------- One-time partition-table bootstrap for OTA --------------------
 *
 * The original firmware had no 'otadata' partition, which makes
 * esp_ota_set_boot_partition() fail with "Failed to set boot partition".
 *
 * This function is called once from ota_upload_handler() when the otadata
 * partition is not found at runtime.  It:
 *   1. Rewrites the partition table at 0x8000 — adding the otadata entry
 *      and a correct MD5 checksum (format verified against gen_esp32part.py).
 *   2. Initialises the otadata sector (slot 0, seq=1 → ota_0) so the
 *      firmware that was just written boots directly after the restart.
 *
 * After esp_restart() the bootloader sees the updated partition table and
 * the device boots the new firmware in ota_0.  All subsequent OTA uploads
 * use the normal esp_ota_set_boot_partition() path.
 *
 * Flash layout this function hard-codes (derived from partitions.csv):
 *   nvs       0x009000  24 KB
 *   phy_init  0x00F000   4 KB
 *   factory   0x010000   1 MB
 *   ota_0     0x110000   1 MB
 *   www       0x210000   1 MB
 *   coredump  0x310000 192 KB
 *   otadata   0x340000   8 KB  ← new
 * -------------------------------------------------------------------------- */

/* Partition table entry — must match esp_partition_info_t in bootloader. */
typedef struct {
    uint16_t magic;       /* 0xAA50 */
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    char     label[16];
    uint32_t flags;
} __attribute__((packed)) ota_pt_entry_t;  /* 32 bytes */

/* otadata slot — must match esp_ota_select_entry_t in bootloader_support. */
typedef struct {
    uint32_t ota_seq;
    uint8_t  seq_label[20];
    uint32_t ota_state;
    uint32_t crc;          /* CRC32 of the preceding 28 bytes */
} __attribute__((packed)) ota_select_entry_t;  /* 32 bytes */

static esp_err_t ota_bootstrap_otadata_partition(void)
{
    /* ---- 1. Build and write the new partition table ---------------------- */

    static const ota_pt_entry_t pt_entries[] = {
        /* magic   type  sub  offset      size       label       flags */
        { 0xAA50U, 0x01, 0x02, 0x009000U, 0x006000U, "nvs",      0U },
        { 0xAA50U, 0x01, 0x01, 0x00F000U, 0x001000U, "phy_init", 0U },
        { 0xAA50U, 0x00, 0x00, 0x010000U, 0x100000U, "factory",  0U },
        { 0xAA50U, 0x00, 0x10, 0x110000U, 0x100000U, "ota_0",    0U },
        { 0xAA50U, 0x01, 0x82, 0x210000U, 0x100000U, "www",      0U },
        { 0xAA50U, 0x01, 0x03, 0x310000U, 0x030000U, "coredump", 0U },
        { 0xAA50U, 0x01, 0x00, 0x340000U, 0x002000U, "otadata",  0U },
    };
    const size_t entries_size = sizeof(pt_entries);   /* 7 * 32 = 224 bytes */

    /* Partition table sector: 4 KB at flash offset 0x8000 */
    uint8_t *pt_buf = malloc(0x1000U);
    if (!pt_buf) return ESP_ERR_NO_MEM;
    memset(pt_buf, 0xFF, 0x1000U);
    memcpy(pt_buf, pt_entries, entries_size);

    /* MD5 entry header: 0xEB 0xEB followed by 14 × 0xFF
     * (bytes 2..15 are already 0xFF from memset — leave them).
     * The MD5 is computed over: all partition entries (224 B)
     *                         + this 16-byte header (magic + 14×FF).
     * This exactly matches gen_esp32part.py behaviour. */
    uint8_t *md5_hdr = pt_buf + entries_size;
    md5_hdr[0] = 0xEBU;
    md5_hdr[1] = 0xEBU;
    /* bytes 2..15 stay 0xFF from the memset above */

    uint8_t md5[16];
    md5_context_t md5_ctx;
    esp_rom_md5_init(&md5_ctx);
    esp_rom_md5_update(&md5_ctx, pt_buf, entries_size + 16U); /* entries + MD5 hdr */
    esp_rom_md5_final(md5, &md5_ctx);
    memcpy(md5_hdr + 16, md5, 16);

    esp_err_t err = esp_flash_erase_region(NULL, 0x8000U, 0x1000U);
    if (err == ESP_OK) err = esp_flash_write(NULL, pt_buf, 0x8000U, 0x1000U);
    free(pt_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA bootstrap: partition table write failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "OTA bootstrap: partition table updated (7 entries + MD5)");

    /* ---- 2. Initialise otadata: slot 0 = seq 1 → ota_0 ------------------ */

    /* otadata sector: 8 KB at flash offset 0x340000.
     * Two slots of 4 KB each.  Slot 0 at offset 0, slot 1 at offset 0x1000.
     * seq=1 → (1-1) % 1 = 0 → ota_0.
     * ota_state = 0xFFFFFFFF (UNDEFINED) is what esp_ota_set_boot_partition()
     * itself writes — bootloader treats it as "boot without restrictions". */
    uint8_t *otd_buf = malloc(0x2000U);
    if (!otd_buf) return ESP_ERR_NO_MEM;
    memset(otd_buf, 0xFF, 0x2000U);

    ota_select_entry_t *slot0 = (ota_select_entry_t *)otd_buf;
    slot0->ota_seq   = 1U;
    memset(slot0->seq_label, 0x00, sizeof(slot0->seq_label));
    slot0->ota_state = 0xFFFFFFFFU;
    slot0->crc = crc32_le(0xFFFFFFFFU, (const uint8_t *)slot0,
                          (uint32_t)((uint8_t *)&slot0->crc - (uint8_t *)slot0));

    err = esp_flash_erase_region(NULL, 0x340000U, 0x2000U);
    if (err == ESP_OK) err = esp_flash_write(NULL, otd_buf, 0x340000U, 0x2000U);
    free(otd_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA bootstrap: otadata write failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "OTA bootstrap: otadata initialised (seq=1 -> ota_0)");
    return ESP_OK;
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                "No OTA partition found - partition table needs updating");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update starting: %d bytes -> partition '%s'",
            req->content_len, ota_partition->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = MIN(remaining, BUFFER_SIZE);
        int received = httpd_req_recv(req, buffer, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA receive error: %d", received);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write error: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write error");
            return ESP_FAIL;
        }
        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                err == ESP_ERR_OTA_VALIDATE_FAILED
                ? "Firmware validation failed - wrong chip or corrupt file?"
                : "OTA finalise failed");
        return ESP_FAIL;
    }

    /* If the otadata partition is missing (device built without it), bootstrap
     * the partition table now.  This is a one-time self-heal: the new PT
     * includes the otadata entry and the otadata sector is initialised to
     * select ota_0 (which already holds the firmware we just wrote).
     * After restart the device boots directly into the new firmware.
     * On all subsequent OTA uploads the normal path below is taken. */
    const esp_partition_t *otadata_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);

    if (otadata_part == NULL) {
        /* Safety check: the bootstrap hard-codes otadata pointing to ota_0.
         * If the firmware somehow landed in a different slot, booting would
         * start the wrong image — refuse and require USB flash instead. */
        if (ota_partition->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0) {
            ESP_LOGE(TAG, "OTA: bootstrap unsafe — target subtype 0x%02x is not ota_0",
                    ota_partition->subtype);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                    "Firmware written but partition table bootstrap is unsafe: target is not ota_0. USB flash required.");
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "OTA: otadata partition missing — bootstrapping partition table");
        err = ota_bootstrap_otadata_partition();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: bootstrap failed (%s) — USB flash required", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                    "Firmware written but partition table update failed. USB flash required.");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "OTA: bootstrap complete — rebooting to new firmware");
        httpd_resp_sendstr(req, "OK");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful, rebooting");
    httpd_resp_sendstr(req, "OK");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t heap_info_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "total_free_bytes", info.total_free_bytes);
    cJSON_AddNumberToObject(root, "total_allocated_bytes", info.total_allocated_bytes);
    cJSON_AddNumberToObject(root, "largest_free_block", info.largest_free_block);
    cJSON_AddNumberToObject(root, "minimum_free_bytes", info.minimum_free_bytes);
    cJSON_AddNumberToObject(root, "allocated_blocks", info.allocated_blocks);
    cJSON_AddNumberToObject(root, "free_blocks", info.free_blocks);
    cJSON_AddNumberToObject(root, "total_blocks", info.total_blocks);

    return json_response(req, root);
}

static esp_err_t file_check_etag_hash(httpd_req_t *req, char *file_hash_path, char *etag, size_t etag_size) {
    struct stat file_hash_stat;
    if (stat(file_hash_path, &file_hash_stat) == -1) {
        // Hash file not created yet
        return ESP_ERR_NOT_FOUND;
    }

    FILE *fd_hash = fopen(file_hash_path, "r+");

    // Ensure hash file was opened
    ERROR_ACTION(TAG, fd_hash == NULL, return ESP_FAIL,
            "Could not open hash file %s (%lu bytes) for reading/updating: %d %s", file_hash_path,
            file_hash_stat.st_size, errno, strerror(errno));

    // Read existing hash
    uint32_t crc;
    int read = fread(&crc, sizeof(crc), 1, fd_hash);
    fclose(fd_hash);
    ERROR_ACTION(TAG, read != 1, return ESP_FAIL,
            "Could not read hash file %s: %d %s", file_hash_path,
            errno, strerror(errno));

    snprintf(etag, etag_size, "\"%08lX\"", crc);

    // Compare to header sent by client
    size_t if_none_match_length = httpd_req_get_hdr_value_len(req, "If-None-Match") + 1;
    if (if_none_match_length > 1) {
        char *if_none_match = malloc(if_none_match_length);
        httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, if_none_match_length);

        bool header_match = strcmp(etag, if_none_match) == 0;

        // Matching ETag, return not modified
        if (header_match) {
            free(if_none_match);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "ETag for file %s sent by client does not match (%s != %s)", file_hash_path, etag, if_none_match);
            free(if_none_match);
            return ESP_ERR_INVALID_CRC;
        }
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t file_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char file_path[FILE_PATH_MAX - strlen(FILE_HASH_SUFFIX)];
    char file_hash_path[FILE_PATH_MAX];
    char gz_file_path[FILE_PATH_MAX + 4]; // extra space for ".gz" suffix
    FILE *fd = NULL, *fd_hash = NULL;
    struct stat file_stat;

    // Extract filename from URL
    char *file_name = get_path_from_uri(file_path, WWW_PARTITION_PATH, req->uri, sizeof(file_path));
    ERROR_ACTION(TAG, file_name == NULL, {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }, "Filename too long")

    // If name has trailing '/', respond with index page
    if (strlen(file_name) > 0 && file_name[strlen(file_name) - 1] == '/' && strlen(file_name) + strlen("index.html") < FILE_PATH_MAX) {
        strcpy(&file_name[strlen(file_name)], "index.html");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }

    // Content type is always determined from the original filename (not .gz)
    set_content_type_from_file(req, file_name);

    // Try gzip-compressed version first - saves ~78% bandwidth and SPIFFS space
    snprintf(gz_file_path, sizeof(gz_file_path), "%s.gz", file_path);
    bool use_gz = (stat(gz_file_path, &file_stat) == 0);

    if (use_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    } else {
        // Fall back to uncompressed
        ERROR_ACTION(TAG, stat(file_path, &file_stat) == -1, {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }, "Could not stat file %s", file_path)
    }

    const char *serve_path = use_gz ? gz_file_path : file_path;

    // Check file hash for browser caching (ETag) - only for uncompressed files
    // to avoid buffer overflow from the combined .gz.crc suffix length
    char etag[8 + 2 + 1] = "";
    if (!use_gz) {
        strcpy(file_hash_path, file_path);
        strcpy(&file_hash_path[strlen(file_hash_path)], FILE_HASH_SUFFIX);
        if (file_check_etag_hash(req, file_hash_path, etag, sizeof(etag)) == ESP_OK) {
            httpd_resp_set_status(req, "304 Not Modified");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        if (strlen(etag) > 0) httpd_resp_set_hdr(req, "ETag", etag);
    }

    fd = fopen(serve_path, "r");
    ERROR_ACTION(TAG, fd == NULL, {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read file");
        return ESP_FAIL;
    }, "Could not read file %s", serve_path)

    ESP_LOGI(TAG, "Sending file %s (%ld bytes)%s", file_name, file_stat.st_size, use_gz ? " [gz]" : "");

    size_t length;
    uint32_t crc = 0;
    do {
        length = fread(buffer, 1, BUFFER_SIZE, fd);

        if (httpd_resp_send_chunk(req, buffer, length) != ESP_OK) {
            ESP_LOGE(TAG, "Failed sending file %s", file_name);
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
            fclose(fd);
            return ESP_FAIL;
        }

        crc = crc32_le(crc, (const uint8_t *)buffer, length);
    } while (length != 0);

    fclose(fd);

    // Store CRC hash for uncompressed files only
    if (!use_gz) {
        fd_hash = fopen(file_hash_path, "w");
        if (fd_hash != NULL) {
            fwrite(&crc, sizeof(crc), 1, fd_hash);
            fclose(fd_hash);
        } else {
            ESP_LOGW(TAG, "Could not open hash file %s for writing: %d %s", file_hash_path, errno, strerror(errno));
        }
    }

    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", app_desc->version);

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    for (int i = 0; i < config_item_count; i++) {
        const config_item_t *item = &config_items[i];

        int64_t int64 = 0;
        uint64_t uint64 = 0;

        size_t length = 0;
        char *string = NULL;

        config_color_t color;
        esp_ip4_addr_t ip;

        switch (item->type) {
            case CONFIG_ITEM_TYPE_STRING:
            case CONFIG_ITEM_TYPE_BLOB:
                // Get length
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, NULL, &length));
                string = calloc(1, length + 1);

                // Get value
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, string, &length));
                string[length] = '\0';
                break;
            case CONFIG_ITEM_TYPE_COLOR:
                // Convert to hex
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &color));
                asprintf(&string, "#%02x%02x%02x", color.values.red, color.values.green, color.values.blue);
                break;
            case CONFIG_ITEM_TYPE_IP:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &ip));
                cJSON *ip_parts = cJSON_AddArrayToObject(root, item->key);
                for (int b = 0; b < 4; b++) {
                    cJSON_AddItemToArray(ip_parts, cJSON_CreateNumber(esp_ip4_addr_get_byte(&ip, b)));
                }

                break;
            case CONFIG_ITEM_TYPE_UINT8:
            case CONFIG_ITEM_TYPE_UINT16:
            case CONFIG_ITEM_TYPE_UINT32:
            case CONFIG_ITEM_TYPE_UINT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &uint64));
                asprintf(&string, "%llu", uint64);
                break;
            case CONFIG_ITEM_TYPE_BOOL:
            case CONFIG_ITEM_TYPE_INT8:
            case CONFIG_ITEM_TYPE_INT16:
            case CONFIG_ITEM_TYPE_INT32:
            case CONFIG_ITEM_TYPE_INT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &int64));
                asprintf(&string, "%lld", int64);
                break;
            default:
                string = calloc(1, 1);
                break;
        }

        if (string != NULL) {
            // Hide secret values that aren't empty
            char *value = item->secret && strlen(string) > 0 ? CONFIG_VALUE_UNCHANGED : string;
            cJSON_AddStringToObject(root, item->key, value);

            free(string);
        }
    }

    return json_response(req, root);
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    int ret = httpd_req_recv(req, buffer, BUFFER_SIZE - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }

        return ESP_FAIL;
    }

    buffer[ret] = '\0';

    cJSON *root = cJSON_Parse(buffer);

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    for (int i = 0; i < config_item_count; i++) {
        config_item_t item = config_items[i];

        if (cJSON_HasObjectItem(root, item.key)) {
            cJSON *entry = cJSON_GetObjectItem(root, item.key);

            size_t length = 0;
            if (cJSON_IsString(entry)) {
                length = strlen(entry->valuestring);

                // Ignore empty primitives
                if (length == 0 && item.type != CONFIG_ITEM_TYPE_BLOB && item.type != CONFIG_ITEM_TYPE_STRING) continue;

                // Ignore unchanged values
                if (strcmp(entry->valuestring, CONFIG_VALUE_UNCHANGED) == 0) continue;
            }

            // TODO: Cleanup
            esp_err_t err;
            if (item.type > CONFIG_ITEM_TYPE_MAX) {
                err = ESP_ERR_INVALID_ARG;
            } else if (item.type == CONFIG_ITEM_TYPE_STRING) {
                err = config_set_str(item.key, entry->valuestring);
            } else if (item.type == CONFIG_ITEM_TYPE_BLOB) {
                err = config_set_blob(item.key, entry->valuestring, length);
            } else if (item.type == CONFIG_ITEM_TYPE_COLOR) {
                bool is_black = strcmp(entry->valuestring, "#000000") == 0;
                config_color_t color;
                color.rgba = strtoul(entry->valuestring + 1, NULL, 16) << 8u;

                if (!is_black && color.rgba == 0) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    // Set alpha to default
                    if (!is_black) color.values.alpha = item.def.color.values.alpha;

                    err = config_set_color(item.key, color);
                }
            } else if (item.type == CONFIG_ITEM_TYPE_IP) {
                uint8_t a[4];

                if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) != 4) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    for (int b = 0; b < 4; b++) {
                        a[b] = (uint8_t) strtoul(cJSON_GetArrayItem(entry, b)->valuestring, NULL, 10);
                    }
                    uint32_t ip = esp_netif_htonl(esp_netif_ip4_makeu32(a[0], a[1], a[2], a[3]));
                    err = config_set_u32(item.key, ip);
                }
            } else {
                bool is_zero = strcmp(entry->valuestring, "0") == 0 || strcmp(entry->valuestring, "0.0") == 0;
                int64_t int64 = strtol(entry->valuestring, NULL, 10);
                uint64_t uint64 = strtoul(entry->valuestring, NULL, 10);

                if (!is_zero && (int64 == 0 || uint64 == 0)) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    switch (item.type) {
                        case CONFIG_ITEM_TYPE_BOOL:
                        case CONFIG_ITEM_TYPE_INT8:
                        case CONFIG_ITEM_TYPE_INT16:
                        case CONFIG_ITEM_TYPE_INT32:
                        case CONFIG_ITEM_TYPE_INT64:
                            err = config_set(&item, &int64);
                            break;
                        case CONFIG_ITEM_TYPE_UINT8:
                        case CONFIG_ITEM_TYPE_UINT16:
                        case CONFIG_ITEM_TYPE_UINT32:
                        case CONFIG_ITEM_TYPE_UINT64:
                            err = config_set(&item, &uint64);
                            break;
                        default:
                            err = ESP_FAIL;
                            break;
                    }
                }
            }

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error setting %s = %s: %d - %s", item.key, entry->valuestring, err, esp_err_to_name(err));
            }
        }
    }

    cJSON_Delete(root);

    config_commit();
    config_restart();

    root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);

    return json_response(req, root);
}

static esp_err_t serial_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    int ret = httpd_req_recv(req, buffer, BUFFER_SIZE - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    buffer[ret] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
        return ESP_FAIL;
    }

    const cJSON *command_item = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (cJSON_IsString(command_item) && (command_item->valuestring != NULL)) {
        const char *command = command_item->valuestring;
        ESP_LOGI(TAG, "Received serial command: %s", command);

        // Append newline characters for the transmission
        char* command_with_newline;
        asprintf(&command_with_newline, "%s\r\n", command);

        // *** Use the project's custom uart_write function ***
        uart_write(command_with_newline, strlen(command_with_newline));

        free(command_with_newline);

        httpd_resp_send(req, "Command sent", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'command' key in JSON");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    // Uptime
    cJSON_AddNumberToObject(root, "uptime", (int) ((double) esp_timer_get_time() / 1000000));

    // Reset reason (why did the device last restart?)
    const char *reset_reason_str;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   reset_reason_str = "POWERON";            break;
        case ESP_RST_EXT:       reset_reason_str = "EXTERNAL";           break;
        case ESP_RST_SW:        reset_reason_str = "SOFTWARE";           break;
        case ESP_RST_PANIC:     reset_reason_str = "PANIC";              break;
        case ESP_RST_INT_WDT:   reset_reason_str = "INTERRUPT_WATCHDOG"; break;
        case ESP_RST_TASK_WDT:  reset_reason_str = "TASK_WATCHDOG";      break;
        case ESP_RST_WDT:       reset_reason_str = "OTHER_WATCHDOG";     break;
        case ESP_RST_DEEPSLEEP: reset_reason_str = "DEEPSLEEP";          break;
        case ESP_RST_BROWNOUT:  reset_reason_str = "BROWNOUT";           break;
        default:                reset_reason_str = "UNKNOWN";            break;
    }
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str);
    cJSON_AddBoolToObject(root, "core_dump_available", core_dump_available() > 0);

    // Heap
    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "total", heap_caps_get_total_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(heap, "free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(heap, "minimum_free", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(heap, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // Streams
    cJSON *streams = cJSON_AddObjectToObject(root, "streams");
    stream_stats_values_t values;
    for (stream_stats_handle_t stats = stream_stats_first(); stats != NULL; stats = stream_stats_next(stats)) {
        stream_stats_values(stats, &values);

        cJSON *stream = cJSON_AddObjectToObject(streams, values.name);
        cJSON *total = cJSON_AddObjectToObject(stream, "total");
        cJSON_AddNumberToObject(total, "in", values.total_in);
        cJSON_AddNumberToObject(total, "out", values.total_out);
        cJSON *rate = cJSON_AddObjectToObject(stream, "rate");
        cJSON_AddNumberToObject(rate, "in", values.rate_in);
        cJSON_AddNumberToObject(rate, "out", values.rate_out);
    }

    // Sockets
    cJSON *sockets = cJSON_AddArrayToObject(root, "sockets");
    for (int s = LWIP_SOCKET_OFFSET; s < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; s++) {
        int err;

        int socktype;
        socklen_t socktype_len = sizeof(socktype);
        err = getsockopt(s, SOL_SOCKET, SO_TYPE, &socktype, &socktype_len);
        if (err < 0) continue;

        cJSON *socket = cJSON_CreateObject();

        cJSON_AddStringToObject(socket, "type", SOCKTYPE_NAME(socktype));

        struct sockaddr_in6 addr;
        socklen_t socklen = sizeof(addr);

        err = getsockname(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "local", sockaddrtostr((struct sockaddr *) &addr));

        err = getpeername(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "peer", sockaddrtostr((struct sockaddr *) &addr));

        cJSON_AddItemToArray(sockets, socket);
    }

    // WiFi
    wifi_ap_status_t ap_status;
    wifi_sta_status_t sta_status;

    wifi_ap_status(&ap_status);
    wifi_sta_status(&sta_status);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");

    cJSON *ap = cJSON_AddObjectToObject(wifi, "ap");
    cJSON_AddBoolToObject(ap, "active", ap_status.active);
    if (ap_status.active) {
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_status.ssid);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_status.authmode));
        cJSON_AddNumberToObject(ap, "devices", ap_status.devices);

        char ip[40];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ap_status.ip4_addr));
        cJSON_AddStringToObject(ap, "ip4", ip);
        snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(ap_status.ip6_addr));
        cJSON_AddStringToObject(ap, "ip6", ip);
    }

    cJSON *sta = cJSON_AddObjectToObject(wifi, "sta");
    cJSON_AddBoolToObject(sta, "active", sta_status.active);
    if (sta_status.active) {
        cJSON_AddBoolToObject(sta, "connected", sta_status.connected);
        if (sta_status.connected) {
            cJSON_AddStringToObject(sta, "ssid", (char *) sta_status.ssid);
            cJSON_AddStringToObject(sta, "authmode", wifi_auth_mode_name(sta_status.authmode));
            cJSON_AddNumberToObject(sta, "rssi", sta_status.rssi);

            char ip[40];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&sta_status.ip4_addr));
            cJSON_AddStringToObject(sta, "ip4", ip);
            snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(sta_status.ip6_addr));
            cJSON_AddStringToObject(sta, "ip6", ip);
        }
    }

    // Internal chip temperature
    float chip_temp_c = 0.0f;
    if (temp_sensor_read_celsius(&chip_temp_c) == ESP_OK) {
        cJSON_AddNumberToObject(root, "chip_temp_c", chip_temp_c);
    }

    return json_response(req, root);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    uint16_t ap_count;
    wifi_ap_record_t *ap_records =  wifi_scan(&ap_count);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        wifi_ap_record_t *ap_record = &ap_records[i];
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddItemToArray(root, ap);
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_record->ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_record->rssi);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_record->authmode));
    }

    free(ap_records);

    return json_response(req, root);
}

static esp_err_t register_uri_handler(httpd_handle_t server, const char *path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *r)) {
    httpd_uri_t uri_config_get = {
            .uri        = path,
            .method     = method,
            .handler    = handler
    };
    return httpd_register_uri_handler(server, &uri_config_get);
}

static httpd_handle_t web_server_start(void)
{
    config_get_primitive(CONF_ITEM(KEY_CONFIG_ADMIN_AUTH), &auth_method);
    if (auth_method == AUTH_METHOD_BASIC) {
        char *username, *password;
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_USERNAME), (void **) &username);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_PASSWORD), (void **) &password);
        basic_authentication = http_auth_basic_header(username, password);
        free(username);
        free(password);
    }

    buffer = malloc(BUFFER_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Could not allocate HTTP response buffer");
        return NULL;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 11;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        register_uri_handler(server, "/config", HTTP_GET, config_get_handler);
        register_uri_handler(server, "/config", HTTP_POST, config_post_handler);
        register_uri_handler(server, "/status", HTTP_GET, status_get_handler);

        register_uri_handler(server, "/log", HTTP_GET, log_get_handler);
        register_uri_handler(server, "/core_dump", HTTP_GET, core_dump_get_handler);
        register_uri_handler(server, "/heap_info", HTTP_GET, heap_info_get_handler);

        register_uri_handler(server, "/wifi/scan", HTTP_GET, wifi_scan_get_handler);

        register_uri_handler(server, "/send_serial", HTTP_POST, serial_post_handler);

        register_uri_handler(server, "/ota", HTTP_GET, ota_page_handler);
        register_uri_handler(server, "/ota", HTTP_POST, ota_upload_handler);

        register_uri_handler(server, "/*", HTTP_GET, file_get_handler);
    }

    if (server == NULL) {
        ESP_LOGE(TAG, "Could not start server");
        return NULL;
    }

    return server;
}

void web_server_init() {
    www_spiffs_init();

    // Internal temperature sensor (ESP32 built-in, ±5°C accuracy)
    temp_sensor_config_t tsens = TSENS_CONFIG_DEFAULT();
    temp_sensor_set_config(tsens);
    temp_sensor_start();

    web_server_start();
}
