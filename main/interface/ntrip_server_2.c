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

#include <stdbool.h>
#include <esp_log.h>
#include <esp_event_base.h>
#include <sys/socket.h>
#include <wifi.h>
#include <tasks.h>
#include <status_led.h>
#include <retry.h>
#include <stream_stats.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include "interface/ntrip.h"
#include "config.h"
#include "util.h"
#include "uart.h"

static const char *TAG = "NTRIP_SERVER_2";

#define BUFFER_SIZE 512

static const int CASTER_READY_BIT = BIT0;
static const int DATA_READY_BIT = BIT1;
static const int DATA_SENT_BIT = BIT2;

static int sock = -1;
static SemaphoreHandle_t sock_mutex = NULL;

static int data_keep_alive;
static EventGroupHandle_t server_event_group;

static status_led_handle_t status_led = NULL;
static stream_stats_handle_t stream_stats = NULL;

static TaskHandle_t server_task = NULL;
static TaskHandle_t sleep_task = NULL;

static void ntrip_server_uart_handler(void* handler_args, esp_event_base_t base, int32_t length, void* buffer) {
    EventBits_t event_bits = xEventGroupGetBits(server_event_group);

    // Reset data availability bit
    if ((event_bits & DATA_READY_BIT) == 0) {
        xEventGroupSetBits(server_event_group, DATA_READY_BIT);

        if (event_bits & DATA_SENT_BIT)
            ESP_LOGI(TAG, "Data received by UART, will now reconnect to caster if disconnected");
    }
    data_keep_alive = 0;

    // Ignore if caster is not connected and ready for data
    if ((event_bits & CASTER_READY_BIT) == 0) return;

    // Caster is connected and some data will be sent
    if ((event_bits & DATA_SENT_BIT) == 0) xEventGroupSetBits(server_event_group, DATA_SENT_BIT);

    /* Snapshot the fd under the mutex — the actual write runs outside it
     * because SO_SNDTIMEO can make write() block for up to 10 s. */
    xSemaphoreTake(sock_mutex, portMAX_DELAY);
    int current_sock = sock;
    xSemaphoreGive(sock_mutex);

    if (current_sock < 0) return;  // sleep task already closed it

    int sent = write(current_sock, buffer, length);
    if (sent < 0) {
        xSemaphoreTake(sock_mutex, portMAX_DELAY);
        if (sock == current_sock) destroy_socket(&sock);  // avoid double-close
        xSemaphoreGive(sock_mutex);
        vTaskResume(server_task);
    } else {
        stream_stats_increment(stream_stats, 0, sent);
    }
}

static void ntrip_server_sleep_task(void *ctx) {
    vTaskSuspend(NULL);

    ESP_LOGI(TAG, "Sleep task stack high water mark: %d bytes remaining", uxTaskGetStackHighWaterMark(NULL) * 4);

    while (true) {
        // If wait time exceeded, clear data ready bit
        if (data_keep_alive >= NTRIP_KEEP_ALIVE_THRESHOLD) {
            EventBits_t bits = xEventGroupGetBits(server_event_group);
            if (bits & CASTER_READY_BIT) {
                // Still connected but no UART data - force reconnect so we don't hold a stale connection
                ESP_LOGW(TAG, "No data received by UART in %d seconds while connected, forcing reconnect", NTRIP_KEEP_ALIVE_THRESHOLD / 1000);
                xEventGroupClearBits(server_event_group, CASTER_READY_BIT);
                xSemaphoreTake(sock_mutex, portMAX_DELAY);
                destroy_socket(&sock);
                xSemaphoreGive(sock_mutex);
                vTaskResume(server_task);
            }
            xEventGroupClearBits(server_event_group, DATA_READY_BIT);
            ESP_LOGW(TAG, "No data received by UART in %d seconds, will not reconnect to caster if disconnected", NTRIP_KEEP_ALIVE_THRESHOLD / 1000);
        }
        data_keep_alive += NTRIP_KEEP_ALIVE_THRESHOLD / 10;
        vTaskDelay(pdMS_TO_TICKS(NTRIP_KEEP_ALIVE_THRESHOLD / 10));
    }
}

static void ntrip_server_task(void *ctx) {
    sock_mutex = xSemaphoreCreateMutex();
    server_event_group = xEventGroupCreate();
    uart_register_read_handler(ntrip_server_uart_handler);
    xTaskCreate(ntrip_server_sleep_task, "ntrip_server_sleep_task", 4096, NULL, TASK_PRIORITY_INTERFACE, &sleep_task);

    config_color_t status_led_color = config_get_color(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_COLOR));
    if (status_led_color.rgba != 0) status_led = status_led_add(status_led_color.rgba, STATUS_LED_FADE, 500, 2000, 0);
    if (status_led != NULL) status_led->active = false;

    stream_stats = stream_stats_new("ntrip_server_2");

    retry_delay_handle_t delay_handle = retry_init(true, 5, 2000, 0);

    while (true) {
        retry_delay(delay_handle);

        // Wait for data to be available
        if ((xEventGroupGetBits(server_event_group) & DATA_READY_BIT) == 0) {
            ESP_LOGI(TAG, "Waiting for UART input to connect to caster");
            uart_nmea("$PESP,NTRIP,SRV2,WAITING");
            xEventGroupWaitBits(server_event_group, DATA_READY_BIT, true, false, portMAX_DELAY);
        }

        vTaskResume(sleep_task);

        wait_for_ip();

        char *buffer = NULL;

        char *host, *mountpoint, *password;
        uint16_t port = 0;
        config_get_primitive(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_PORT), &port);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_HOST), (void **) &host);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_PASSWORD), (void **) &password);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_MOUNTPOINT), (void **) &mountpoint);

        ERROR_ACTION(TAG, host == NULL || password == NULL || mountpoint == NULL, goto _error,
                "Could not read config from NVS (host=%s, mountpoint=%s)", host ? host : "NULL", mountpoint ? mountpoint : "NULL");

        ESP_LOGI(TAG, "Connecting to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV2,CONNECTING,%s:%d,%s", host, port, mountpoint);
        sock = connect_socket(host, port, SOCK_STREAM);
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_RESOLVE, goto _error, "Could not resolve host");
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_CONNECT, goto _error, "Could not connect to host");
        ERROR_ACTION(TAG, sock < 0, goto _error, "Could not configure socket options");

        buffer = malloc(BUFFER_SIZE);

        snprintf(buffer, BUFFER_SIZE, "SOURCE %s /%s" NEWLINE \
                "Source-Agent: NTRIP %s/%s" NEWLINE \
                NEWLINE, password, mountpoint, NTRIP_SERVER_NAME, &esp_app_get_description()->version[1]);

        int err = write(sock, buffer, strlen(buffer));
        ERROR_ACTION(TAG, err < 0, goto _error, "Could not send request to caster: %d %s", errno, strerror(errno));

        int len = read(sock, buffer, BUFFER_SIZE - 1);
        ERROR_ACTION(TAG, len <= 0, goto _error, "Could not receive response from caster: %s",
                (errno == EAGAIN || errno == EWOULDBLOCK) ? "Timed out waiting for response (10s)" : strerror(errno));
        buffer[len] = '\0';

        char *status = extract_http_header(buffer, "");
        ERROR_ACTION(TAG, status == NULL || !ntrip_response_ok(status), free(status); goto _error,
                "Could not connect to mountpoint: %s", status == NULL ? "HTTP response malformed" : status);
        free(status);

        ESP_LOGI(TAG, "Successfully connected to %s:%d/%s", host, port, mountpoint);
        ESP_LOGI(TAG, "Stack high water mark: %d bytes remaining", uxTaskGetStackHighWaterMark(NULL) * 4);
        uart_nmea("$PESP,NTRIP,SRV2,CONNECTED,%s:%d,%s", host, port, mountpoint);

        retry_reset(delay_handle);

        if (status_led != NULL) status_led->active = true;

        // Connected
        xEventGroupSetBits(server_event_group, CASTER_READY_BIT);

        // Await disconnect from UART handler
        vTaskSuspend(NULL);

        // Disconnected
        xEventGroupClearBits(server_event_group, CASTER_READY_BIT | DATA_SENT_BIT);

        if (status_led != NULL) status_led->active = false;

        ESP_LOGW(TAG, "Disconnected from %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,SRV2,DISCONNECTED,%s:%d,%s", host, port, mountpoint);

        _error:
        vTaskSuspend(sleep_task);

        xSemaphoreTake(sock_mutex, portMAX_DELAY);
        destroy_socket(&sock);
        xSemaphoreGive(sock_mutex);

        free(buffer);
        free(host);
        free(mountpoint);
        free(password);
    }
}

void ntrip_server_2_init() {
    if (!config_get_bool1(CONF_ITEM(KEY_CONFIG_NTRIP_SERVER_2_ACTIVE))) return;

    xTaskCreate(ntrip_server_task, "ntrip_server_2_task", 8192, NULL, TASK_PRIORITY_INTERFACE, &server_task);
}
