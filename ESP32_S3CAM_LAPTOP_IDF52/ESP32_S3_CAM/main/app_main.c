
/*
 * ESP32-S3-CAM
 * ESP-IDF 5.2
 *
 * Attendance and face enrollment bridge:
 *
 * ESP32-S3-CAM <--- Wi-Fi ---> Python AI Server
 * ESP32-S3-CAM <--- UART ---> ESP32 MAIN
 *
 * Features:
 * 1. Capture OV3660 JPEG frames.
 * 2. Send images to the Python AI server.
 * 3. Receive CNN face recognition results.
 * 4. Send employee ID and RGB565 image to MAIN.
 * 5. Verify website enrollment requests through MAIN.
 * 6. Synchronize enrolled face IDs to MAIN.
 * 7. Forward employee face deletion requests.
 *
 * MAIN and S3 UART protocol:
 * UTST / TYPE / SEQUENCE / LENGTH / CRC32 / PAYLOAD
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/uart.h"

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_err.h"

#include "nvs_flash.h"

#include "sdkconfig.h"
#include "face_config.h"
#include "face_uart_link.h"

/* GPIO43 and GPIO44 are reserved for UART1. */

#ifndef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "Configure S3 console as USB Serial/JTAG in menuconfig"
#endif

static const char *TAG = "FACE_S3";

/* ============================================================
 * DEFINITIONS
 * ============================================================ */

#define FACE_UART UART_NUM_1

#define SERVER_MAGIC "FCR1"

#define SERVER_HEADER_LEN 15

#define SERVER_STATUS_MATCH 1
#define SERVER_STATUS_UNKNOWN 2

#define MAX_EMPLOYEE_ID_TEXT 10

#define SERVER_RESPONSE_MAX \
    (SERVER_HEADER_LEN + MAX_EMPLOYEE_ID_TEXT + FACE_THUMB_BYTES)

_Static_assert(
    FACE_THUMB_BYTES == FL_MAX_IMAGE_BYTES,
    "Expected RGB565 image 96x96"
);

/* ============================================================
 * GLOBAL STATE
 * ============================================================ */

static uint8_t *server_response_buffer;
static uint8_t *face_uart_payload;

static SemaphoreHandle_t uart_tx_mutex;
static QueueHandle_t uart_reply_queue;
static QueueHandle_t employee_decision_queue;
static QueueHandle_t control_queue;

static uint32_t next_uart_seq = 1;

static volatile bool uart_link_ready = false;

static volatile bool person_present = true;

static bool website_enrollment_active = false;

static unsigned unknown_face_streak = 0;

static int64_t last_unknown_report_us = 0;

/* ============================================================
 * UART STRUCTURES
 * ============================================================ */

typedef struct {
    uint32_t type;
    uint32_t seq;
    uint32_t crc;
} uart_reply_t;

typedef struct {
    uint16_t id;
    char token[9];
    bool exists;
} employee_decision_t;

typedef enum {
    CONTROL_DELETE_FACE = 1
} control_type_t;

typedef struct {
    control_type_t type;
    uint16_t id;
} control_command_t;

/* ============================================================
 * LITTLE-ENDIAN HELPERS
 * ============================================================ */

static uint16_t load_u16(const uint8_t *p)
{
    return (uint16_t)p[0] |
           ((uint16_t)p[1] << 8);
}

static uint32_t load_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ============================================================
 * UART TRANSMISSION
 * ============================================================ */

static bool uart_send(
    uint32_t type,
    uint32_t seq,
    const void *payload,
    uint32_t length,
    uint32_t crc)
{
    return fl_send_frame(
        FACE_UART,
        uart_tx_mutex,
        type,
        seq,
        payload,
        length,
        crc
    );
}

/* Wait for matching ACK/READY from MAIN. */

static bool uart_wait_reply(
    uint32_t wanted_type,
    uint32_t wanted_seq,
    uint32_t wanted_crc,
    uint32_t timeout_ms)
{
    uart_reply_t reply;

    int64_t deadline =
        esp_timer_get_time() +
        (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {

        if (xQueueReceive(
                uart_reply_queue,
                &reply,
                pdMS_TO_TICKS(30)
            ) != pdTRUE) {
            continue;
        }

        if (
            reply.seq == wanted_seq &&
            reply.type == FL_TYPE_NACK
        ) {
            return false;
        }

        if (
            reply.type == wanted_type &&
            reply.seq == wanted_seq &&
            reply.crc == wanted_crc
        ) {
            return true;
        }
    }

    return false;
}

/* ============================================================
 * UART HANDSHAKE
 * ============================================================ */

static bool uart_handshake(void)
{
    uart_link_ready = false;

    for (unsigned attempt = 1;
         attempt <= 3;
         ++attempt) {

        xQueueReset(uart_reply_queue);

        bool sent = uart_send(
            FL_TYPE_HELLO,
            0,
            NULL,
            0,
            0
        );

        if (
            sent &&
            uart_wait_reply(
                FL_TYPE_READY,
                0,
                0,
                1800
            )
        ) {
            next_uart_seq = 1;
            uart_link_ready = true;

            ESP_LOGI(
                TAG,
                "UART HANDSHAKE PASS"
            );

            return true;
        }

        ESP_LOGW(
            TAG,
            "UART HANDSHAKE attempt=%u failed",
            attempt
        );
    }

    return false;
}

/* ============================================================
 * RELIABLE UART EVENT
 * ============================================================ */

static bool uart_send_event(
    uint32_t type,
    const uint8_t *payload,
    uint32_t length)
{
    if (
        !payload ||
        length == 0 ||
        length > FL_MAX_PAYLOAD
    ) {
        return false;
    }

    if (
        !uart_link_ready &&
        !uart_handshake()
    ) {
        return false;
    }

    uint32_t seq = next_uart_seq++;

    uint32_t crc = fl_crc32(
        payload,
        length
    );

    for (unsigned attempt = 1;
         attempt <= 3;
         ++attempt) {

        xQueueReset(uart_reply_queue);

        bool sent = uart_send(
            type,
            seq,
            payload,
            length,
            crc
        );

        if (
            sent &&
            uart_wait_reply(
                FL_TYPE_ACK,
                seq,
                crc,
                2500
            )
        ) {
            if (attempt > 1) {
                ESP_LOGW(
                    TAG,
                    "UART recovered seq=%" PRIu32
                    " attempt=%u",
                    seq,
                    attempt
                );
            }

            return true;
        }

        ESP_LOGW(
            TAG,
            "UART no ACK type=%" PRIu32
            " seq=%" PRIu32
            " attempt=%u",
            type,
            seq,
            attempt
        );
    }

    uart_link_ready = false;

    ESP_LOGE(
        TAG,
        "UART EVENT FAILED seq=%" PRIu32,
        seq
    );

    return false;
}

static bool uart_send_status(const char *text)
{
    if (!text)
        return false;

    size_t length = strlen(text);

    if (
        length == 0 ||
        length > 110
    ) {
        return false;
    }

    return uart_send_event(
        FL_TYPE_STATUS,
        (const uint8_t *)text,
        (uint32_t)length
    );
}

/* ============================================================
 * UART COMMAND RECEIVER
 *
 * Only this FreeRTOS task reads UART1.
 * ============================================================ */

static void uart_receiver_task(void *arg)
{
    (void)arg;

    uint8_t payload[128];
    fl_header_t header;

    uint32_t last_command_seq = 0;
    uint32_t last_command_crc = 0;

    while (true) {

        fl_rx_result_t result = fl_read_frame(
            FACE_UART,
            &header,
            payload,
            sizeof(payload),
            1000,
            1500
        );

        if (result == FL_RX_NONE)
            continue;

        if (result != FL_RX_OK) {

            ESP_LOGW(
                TAG,
                "UART RX error=%d",
                (int)result
            );

            if (
                header.type == FL_TYPE_COMMAND &&
                header.seq != 0
            ) {
                uart_send(
                    FL_TYPE_NACK,
                    header.seq,
                    NULL,
                    0,
                    0
                );
            }

            if (
                result == FL_RX_BAD_HEADER ||
                result == FL_RX_INCOMPLETE
            ) {
                uart_flush_input(FACE_UART);
            }

            continue;
        }

        /* Replies to S3 -> MAIN events. */

        if (
            header.type == FL_TYPE_ACK ||
            header.type == FL_TYPE_NACK ||
            header.type == FL_TYPE_READY
        ) {
            uart_reply_t reply = {
                .type = header.type,
                .seq = header.seq,
                .crc = header.crc
            };

            xQueueSend(
                uart_reply_queue,
                &reply,
                0
            );

            continue;
        }

        if (header.type != FL_TYPE_COMMAND)
            continue;

        if (
            header.len == 0 ||
            header.len >= sizeof(payload)
        ) {
            uart_send(
                FL_TYPE_NACK,
                header.seq,
                NULL,
                0,
                0
            );

            continue;
        }

        payload[header.len] = '\0';

        const char *command =
            (const char *)payload;

        bool duplicate =
            header.seq == last_command_seq &&
            header.crc == last_command_crc;

        bool accepted = false;

        /* -----------------------------------------------
         * Duplicate command
         * ----------------------------------------------- */

        if (duplicate) {

            accepted = true;

        /* -----------------------------------------------
         * Website employee confirmation
         * ----------------------------------------------- */

        } else if (
            strncmp(command, "EMP_OK:", 7) == 0 ||
            strncmp(command, "EMP_NO:", 7) == 0
        ) {

            unsigned id = 0;
            char token[9] = {0};
            int end_pos = 0;

            if (
                sscanf(
                    command + 7,
                    "%u:%8[0-9a-f]%n",
                    &id,
                    token,
                    &end_pos
                ) == 2 &&
                command[7 + end_pos] == '\0' &&
                strlen(token) == 8 &&
                id > 0 &&
                id <= 65535
            ) {

                employee_decision_t decision = {
                    .id = (uint16_t)id,
                    .exists =
                        strncmp(
                            command,
                            "EMP_OK:",
                            7
                        ) == 0
                };

                memcpy(
                    decision.token,
                    token,
                    sizeof(decision.token)
                );

                accepted = xQueueSend(
                    employee_decision_queue,
                    &decision,
                    0
                ) == pdTRUE;

                ESP_LOGI(
                    TAG,
                    "MAIN employee check ID=%u exists=%d",
                    id,
                    decision.exists
                );
            }

        /* -----------------------------------------------
         * LD2410 presence
         * ----------------------------------------------- */

        } else if (
            strcmp(command, "PRESENCE:1") == 0
        ) {

            person_present = true;
            accepted = true;

        } else if (
            strcmp(command, "PRESENCE:0") == 0
        ) {

            person_present = false;
            accepted = true;

        /* -----------------------------------------------
         * Delete face from Python server
         * ----------------------------------------------- */

        } else if (
            strncmp(command, "FACE_DELETE:", 12) == 0
        ) {

            const char *number = command + 12;

            unsigned id = 0;
            bool valid = true;

            if (
                !*number ||
                strlen(number) > 5
            ) {
                valid = false;
            }

            for (
                const char *p = number;
                valid && *p;
                ++p
            ) {
                if (*p < '0' || *p > '9') {
                    valid = false;
                    break;
                }

                id =
                    id * 10 +
                    (unsigned)(*p - '0');
            }

            if (
                valid &&
                id > 0 &&
                id <= 65535
            ) {

                control_command_t task = {
                    .type = CONTROL_DELETE_FACE,
                    .id = (uint16_t)id
                };

                accepted = xQueueSend(
                    control_queue,
                    &task,
                    0
                ) == pdTRUE;
            }
        }

        if (accepted) {

            last_command_seq = header.seq;
            last_command_crc = header.crc;

            uart_send(
                FL_TYPE_ACK,
                header.seq,
                NULL,
                0,
                header.crc
            );

        } else {

            ESP_LOGW(
                TAG,
                "Unsupported UART command: %s",
                command
            );

            uart_send(
                FL_TYPE_NACK,
                header.seq,
                NULL,
                0,
                0
            );
        }
    }
}

/* ============================================================
 * TRANSMIT FACE RESULT TO MAIN
 * ============================================================ */

static void uart_send_face(
    const char *employee_id,
    const uint8_t *pixels,
    size_t image_length)
{
    if (
        !employee_id ||
        !pixels ||
        image_length != FL_MAX_IMAGE_BYTES ||
        !face_uart_payload
    ) {
        return;
    }

    char *end = NULL;

    unsigned long id = strtoul(
        employee_id,
        &end,
        10
    );

    if (
        end == employee_id ||
        *end != '\0' ||
        id == 0 ||
        id > 65535
    ) {
        return;
    }

    /* Two-byte employee ID. */

    face_uart_payload[0] =
        (uint8_t)id;

    face_uart_payload[1] =
        (uint8_t)(id >> 8);

    /* 18432-byte RGB565 image. */

    memcpy(
        face_uart_payload + 2,
        pixels,
        image_length
    );

    bool success = uart_send_event(
        FL_TYPE_FACE,
        face_uart_payload,
        (uint32_t)image_length + 2
    );

    if (success) {

        ESP_LOGI(
            TAG,
            "FACE UART ACK id=%lu bytes=%u",
            id,
            (unsigned)image_length
        );

    } else {

        ESP_LOGE(
            TAG,
            "FACE UART NOT ACKED id=%lu",
            id
        );
    }
}

/* ============================================================
 * DEVICE HTTP CLIENT
 *
 * GET and POST commands used for:
 * - employee verification
 * - face roster synchronization
 * - face deletion
 * ============================================================ */

static bool device_http(
    const char *endpoint,
    const char *body,
    char *output,
    size_t output_capacity)
{
    if (
        !endpoint ||
        !output ||
        output_capacity < 2
    ) {
        return false;
    }

    char url[192];

    int written = snprintf(
        url,
        sizeof(url),
        "%s%s",
        FACE_SERVER_BASE_URL,
        endpoint
    );

    if (
        written <= 0 ||
        written >= (int)sizeof(url)
    ) {
        return false;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
        .keep_alive_enable = false
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (!client)
        return false;

    esp_http_client_set_header(
        client,
        "X-Device-Key",
        FACE_DEVICE_KEY
    );

    int body_length =
        body ? (int)strlen(body) : 0;

    if (body) {

        esp_http_client_set_method(
            client,
            HTTP_METHOD_POST
        );

        esp_http_client_set_header(
            client,
            "Content-Type",
            "application/json"
        );

    } else {

        esp_http_client_set_method(
            client,
            HTTP_METHOD_GET
        );
    }

    bool success = false;

    if (
        esp_http_client_open(
            client,
            body_length
        ) != ESP_OK
    ) {
        goto cleanup;
    }

    if (body) {

        int sent = esp_http_client_write(
            client,
            body,
            body_length
        );

        if (sent != body_length)
            goto cleanup;
    }

    if (
        esp_http_client_fetch_headers(
            client
        ) < 0
    ) {
        goto cleanup;
    }

    if (
        esp_http_client_get_status_code(
            client
        ) != 200
    ) {
        goto cleanup;
    }

    size_t used = 0;

    while (used < output_capacity - 1) {

        int n = esp_http_client_read(
            client,
            output + used,
            (int)(output_capacity - 1 - used)
        );

        if (n <= 0)
            break;

        used += (size_t)n;
    }

    output[used] = '\0';

    success = used > 0;

cleanup:

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return success;
}

/* ============================================================
 * WEBSITE ENROLLMENT CAPTURE MODE
 *
 * When website registration is active, camera frames
 * must continue to be captured regardless of LD2410C.
 * ============================================================ */

static void poll_website_capture_mode(void)
{
    static int64_t next_poll_us = 0;

    int64_t now = esp_timer_get_time();

    if (now < next_poll_us)
        return;

    next_poll_us = now + 1400000LL;

    char reply[32];

    if (
        device_http(
            "/api/device/enroll/mode",
            NULL,
            reply,
            sizeof(reply)
        )
    ) {
        website_enrollment_active =
            strcmp(reply, "ACTIVE:1\n") == 0;
    }
}

/* ============================================================
 * WEBSITE REGISTRATION REQUEST
 *
 * Website -> Server -> S3 -> MAIN.
 * MAIN verifies local employee ID.
 * ============================================================ */

static void poll_website_registration(void)
{
    static int64_t next_poll_us = 0;
    static int64_t last_sent_us = 0;
    static char last_token[9] = {0};

    int64_t now = esp_timer_get_time();

    if (now < next_poll_us)
        return;

    next_poll_us = now + 1500000LL;

    char reply[96];

    if (
        !device_http(
            "/api/device/enroll/pending",
            NULL,
            reply,
            sizeof(reply)
        )
    ) {
        return;
    }

    unsigned id = 0;
    char token[9] = {0};
    int end_pos = 0;

    if (
        sscanf(
            reply,
            "CHECK:%u:%8[0-9a-f]%n",
            &id,
            token,
            &end_pos
        ) != 2 ||
        strlen(token) != 8 ||
        (
            reply[end_pos] != '\n' &&
            reply[end_pos] != '\0'
        ) ||
        id == 0 ||
        id > 65535
    ) {
        return;
    }

    /* Avoid flooding MAIN with repeated checks. */

    if (
        strcmp(last_token, token) == 0 &&
        now - last_sent_us < 4000000LL
    ) {
        return;
    }

    char message[48];

    snprintf(
        message,
        sizeof(message),
        "CHECK_EMP:%u:%s",
        id,
        token
    );

    if (uart_send_status(message)) {

        memcpy(
            last_token,
            token,
            sizeof(last_token)
        );

        last_sent_us = now;

        ESP_LOGI(
            TAG,
            "Website checking employee ID=%u",
            id
        );
    }
}

/* ============================================================
 * SEND MAIN'S DECISION BACK TO SERVER
 * ============================================================ */

static void submit_employee_decision(void)
{
    static bool pending = false;
    static employee_decision_t decision;
    static int64_t retry_after_us = 0;

    if (
        esp_timer_get_time() < retry_after_us
    ) {
        return;
    }

    if (!pending) {

        pending = xQueueReceive(
            employee_decision_queue,
            &decision,
            0
        ) == pdTRUE;
    }

    if (!pending)
        return;

    char body[120];
    char reply[128];

    snprintf(
        body,
        sizeof(body),
        "{\"id\":\"%u\","
        "\"token\":\"%s\","
        "\"exists\":%s}",
        (unsigned)decision.id,
        decision.token,
        decision.exists ? "true" : "false"
    );

    if (
        device_http(
            "/api/device/enroll/decision",
            body,
            reply,
            sizeof(reply)
        )
    ) {

        ESP_LOGI(
            TAG,
            "MAIN verification delivered ID=%u",
            decision.id
        );

        pending = false;

    } else {

        retry_after_us =
            esp_timer_get_time() + 2000000LL;
    }
}

/* ============================================================
 * SYNCHRONIZE SERVER FACE ROSTER
 *
 * Server returns:
 * IDS:101,102,103\n
 *
 * S3 sends:
 * FACE_SYNC_BEGIN:generation:count
 * FACE_SYNC_ID:id
 * FACE_SYNC_END:generation:count
 *
 * MAIN commits only a complete roster.
 * ============================================================ */

static void synchronize_face_roster(void)
{
    static int64_t next_sync_us = 0;
    static uint32_t generation = 0;

    static char text[4096];
    static uint16_t ids[512];

    int64_t now = esp_timer_get_time();

    if (now < next_sync_us)
        return;

    next_sync_us = now + 7000000LL;

    if (
        !device_http(
            "/api/device/face/roster",
            NULL,
            text,
            sizeof(text)
        )
    ) {
        return;
    }

    size_t length = strlen(text);

    if (
        length < 5 ||
        strncmp(text, "IDS:", 4) != 0 ||
        text[length - 1] != '\n'
    ) {
        return;
    }

    text[length - 1] = '\0';

    unsigned count = 0;

    if (text[4] != '\0') {

        char *save = NULL;

        char *item = strtok_r(
            text + 4,
            ",",
            &save
        );

        while (item) {

            if (count >= 512)
                return;

            char *end = NULL;

            unsigned long id = strtoul(
                item,
                &end,
                10
            );

            if (
                end == item ||
                *end != '\0' ||
                id == 0 ||
                id > 65535 ||
                (
                    count > 0 &&
                    id <= ids[count - 1]
                )
            ) {
                return;
            }

            ids[count++] = (uint16_t)id;

            item = strtok_r(
                NULL,
                ",",
                &save
            );
        }
    }

    uint32_t current_gen = ++generation;

    char message[64];

    snprintf(
        message,
        sizeof(message),
        "FACE_SYNC_BEGIN:%" PRIu32 ":%u",
        current_gen,
        count
    );

    if (!uart_send_status(message))
        return;

    for (unsigned i = 0; i < count; ++i) {

        snprintf(
            message,
            sizeof(message),
            "FACE_SYNC_ID:%u",
            (unsigned)ids[i]
        );

        if (!uart_send_status(message))
            return;
    }

    snprintf(
        message,
        sizeof(message),
        "FACE_SYNC_END:%" PRIu32 ":%u",
        current_gen,
        count
    );

    if (!uart_send_status(message))
        return;

    ESP_LOGI(
        TAG,
        "FACE roster synchronized: %u IDs",
        count
    );
}

/* ============================================================
 * DELETE FACE ON PYTHON SERVER
 * ============================================================ */

static void process_control_command(
    control_command_t command)
{
    if (
        command.type != CONTROL_DELETE_FACE
    ) {
        return;
    }

    char body[48];

    snprintf(
        body,
        sizeof(body),
        "{\"id\":\"%u\"}",
        (unsigned)command.id
    );

    char reply[96];

    if (
        device_http(
            "/api/device/face/delete",
            body,
            reply,
            sizeof(reply)
        ) &&
        strncmp(
            reply,
            "DELETED:",
            8
        ) == 0
    ) {

        char message[48];

        snprintf(
            message,
            sizeof(message),
            "FACE_DELETED:%u",
            (unsigned)command.id
        );

        uart_send_status(message);

        ESP_LOGI(
            TAG,
            "Face deleted on server ID=%u",
            command.id
        );

    } else {

        ESP_LOGW(
            TAG,
            "Face delete pending ID=%u",
            command.id
        );
    }
}

/* ============================================================
 * WIFI CALLBACK
 * ============================================================ */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t base,
    int32_t event,
    void *data)
{
    (void)arg;

    if (
        base == WIFI_EVENT &&
        event == WIFI_EVENT_STA_DISCONNECTED
    ) {

        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected"
        );

        esp_wifi_connect();

    } else if (
        base == IP_EVENT &&
        event == IP_EVENT_STA_GOT_IP
    ) {

        ip_event_got_ip_t *ip =
            (ip_event_got_ip_t *)data;

        ESP_LOGI(
            TAG,
            "Camera IP: " IPSTR,
            IP2STR(&ip->ip_info.ip)
        );
    }
}

/* ============================================================
 * WIFI INITIALIZATION
 * ============================================================ */

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&init_config)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED,
            wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL
        )
    );

    wifi_config_t config = {0};

    snprintf(
        (char *)config.sta.ssid,
        sizeof(config.sta.ssid),
        "%s",
        FACE_WIFI_SSID
    );

    snprintf(
        (char *)config.sta.password,
        sizeof(config.sta.password),
        "%s",
        FACE_WIFI_PASSWORD
    );

    config.sta.threshold.authmode =
        WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &config
        )
    );

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(
        esp_wifi_set_ps(WIFI_PS_NONE)
    );

    esp_wifi_connect();
}

/* ============================================================
 * OV3660 CAMERA INITIALIZATION
 * ============================================================ */

static esp_err_t camera_init(void)
{
    bool psram =
        heap_caps_get_total_size(
            MALLOC_CAP_SPIRAM
        ) > 0;

    ESP_LOGI(
        TAG,
        "PSRAM bytes=%u",
        (unsigned)heap_caps_get_total_size(
            MALLOC_CAP_SPIRAM
        )
    );

    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,

        .pin_xclk = CAM_PIN_XCLK,

        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d0 = CAM_PIN_D0,
        .pin_d1 = CAM_PIN_D1,
        .pin_d2 = CAM_PIN_D2,
        .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4,
        .pin_d5 = CAM_PIN_D5,
        .pin_d6 = CAM_PIN_D6,
        .pin_d7 = CAM_PIN_D7,

        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 10000000,

        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,

        .frame_size =
            psram ? FRAMESIZE_VGA : FRAMESIZE_QVGA,

        .jpeg_quality =
            psram ? 10 : 13,

        .fb_count =
            psram ? 2 : 1,

        .fb_location =
            psram ?
            CAMERA_FB_IN_PSRAM :
            CAMERA_FB_IN_DRAM,

        .grab_mode =
            psram ?
            CAMERA_GRAB_LATEST :
            CAMERA_GRAB_WHEN_EMPTY
    };

    esp_err_t result =
        esp_camera_init(&config);

    if (result != ESP_OK)
        return result;

    sensor_t *sensor =
        esp_camera_sensor_get();

    if (sensor) {

        sensor->set_vflip(
            sensor,
            FACE_CAM_VERTICAL_FLIP
        );

        sensor->set_hmirror(
            sensor,
            FACE_CAM_HORIZONTAL_MIRROR
        );

        sensor->set_brightness(
            sensor,
            1
        );

        sensor->set_saturation(
            sensor,
            -1
        );
    }

    return ESP_OK;
}

/* ============================================================
 * UART INITIALIZATION
 * ============================================================ */

static void uart_init(void)
{
    uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(
        uart_param_config(
            FACE_UART,
            &config
        )
    );

    /* S3 TX43 -> MAIN RX32
     * S3 RX44 <- MAIN TX33
     */

    ESP_ERROR_CHECK(
        uart_set_pin(
            FACE_UART,
            GPIO_NUM_43,
            GPIO_NUM_44,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    ESP_ERROR_CHECK(
        uart_driver_install(
            FACE_UART,
            4096,
            4096,
            0,
            NULL,
            0
        )
    );
}

/* ============================================================
 * READ COMPLETE HTTP BODY
 * ============================================================ */

static bool http_read_exact(
    esp_http_client_handle_t client,
    uint8_t *buffer,
    size_t length)
{
    size_t received = 0;

    while (received < length) {

        int n = esp_http_client_read(
            client,
            (char *)buffer + received,
            (int)(length - received)
        );

        if (n <= 0)
            return false;

        received += (size_t)n;
    }

    return true;
}

/* ============================================================
 * SEND CAMERA JPEG TO SERVER
 *
 * Server response:
 *
 * FCR1
 * status:      1 byte
 * ID length:   uint16 little-endian
 * image width: uint16 little-endian
 * image height:uint16 little-endian
 * image bytes: uint32 little-endian
 * ID string
 * RGB565 thumbnail
 * ============================================================ */

static void send_camera_frame(
    camera_fb_t *frame)
{
    if (
        !frame ||
        !frame->buf ||
        !frame->len
    ) {
        return;
    }

    esp_http_client_config_t config = {
        .url = FACE_SERVER_URL,
        .timeout_ms = 6000,
        .keep_alive_enable = false
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (!client)
        return;

    esp_http_client_set_method(
        client,
        HTTP_METHOD_POST
    );

    esp_http_client_set_header(
        client,
        "Content-Type",
        "image/jpeg"
    );

    esp_http_client_set_header(
        client,
        "X-Device-Key",
        FACE_DEVICE_KEY
    );

    if (
        esp_http_client_open(
            client,
            (int)frame->len
        ) != ESP_OK
    ) {
        goto cleanup;
    }

    size_t sent = 0;

    while (sent < frame->len) {

        int n = esp_http_client_write(
            client,
            (const char *)frame->buf + sent,
            (int)(frame->len - sent)
        );

        if (n <= 0)
            goto cleanup;

        sent += (size_t)n;
    }

    if (
        esp_http_client_fetch_headers(
            client
        ) < 0
    ) {
        goto cleanup;
    }

    if (
        esp_http_client_get_status_code(
            client
        ) != 200
    ) {
        goto cleanup;
    }

    if (
        !http_read_exact(
            client,
            server_response_buffer,
            SERVER_HEADER_LEN
        )
    ) {
        goto cleanup;
    }

    if (
        memcmp(
            server_response_buffer,
            SERVER_MAGIC,
            4
        ) != 0
    ) {
        goto cleanup;
    }

    uint8_t status =
        server_response_buffer[4];

    uint16_t id_length =
        load_u16(
            server_response_buffer + 5
        );

    uint16_t image_width =
        load_u16(
            server_response_buffer + 7
        );

    uint16_t image_height =
        load_u16(
            server_response_buffer + 9
        );

    uint32_t image_length =
        load_u32(
            server_response_buffer + 11
        );

    /* -----------------------------------------------
     * Face detected but unknown
     * ----------------------------------------------- */

    if (
        status == SERVER_STATUS_UNKNOWN &&
        !website_enrollment_active
    ) {

        if (unknown_face_streak < 5)
            unknown_face_streak++;

        int64_t now = esp_timer_get_time();

        if (
            unknown_face_streak >= 3 &&
            now - last_unknown_report_us >
                5000000LL
        ) {

            uart_send_status("FACE_UNKNOWN");

            last_unknown_report_us = now;

            unknown_face_streak = 0;
        }

    } else {

        unknown_face_streak = 0;
    }

    /* -----------------------------------------------
     * Face recognition match
     * ----------------------------------------------- */

    if (
        status != SERVER_STATUS_MATCH ||
        id_length == 0 ||
        id_length > MAX_EMPLOYEE_ID_TEXT ||
        image_width != FACE_THUMB_W ||
        image_height != FACE_THUMB_H ||
        image_length != FACE_THUMB_BYTES
    ) {
        goto cleanup;
    }

    if (
        id_length + image_length >
        SERVER_RESPONSE_MAX - SERVER_HEADER_LEN
    ) {
        goto cleanup;
    }

    if (
        !http_read_exact(
            client,
            server_response_buffer +
                SERVER_HEADER_LEN,
            id_length + image_length
        )
    ) {
        goto cleanup;
    }

    char employee_id[
        MAX_EMPLOYEE_ID_TEXT + 1
    ];

    memcpy(
        employee_id,
        server_response_buffer +
            SERVER_HEADER_LEN,
        id_length
    );

    employee_id[id_length] = '\0';

    bool valid = true;

    for (unsigned i = 0;
         i < id_length;
         ++i) {

        if (
            employee_id[i] < '0' ||
            employee_id[i] > '9'
        ) {
            valid = false;
            break;
        }
    }

    if (valid) {

        uart_send_face(
            employee_id,
            server_response_buffer +
                SERVER_HEADER_LEN +
                id_length,
            image_length
        );
    }

cleanup:

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* ============================================================
 * MAIN ENTRY POINT
 * ============================================================ */

void app_main(void)
{
    /* --------------------------------------------------------
     * NVS
     * -------------------------------------------------------- */

    esp_err_t err = nvs_flash_init();

    if (
        err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {

        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    /* --------------------------------------------------------
     * UART hardware
     * -------------------------------------------------------- */

    uart_init();

    /* --------------------------------------------------------
     * Queue and memory
     * -------------------------------------------------------- */

    uart_tx_mutex = xSemaphoreCreateMutex();

    uart_reply_queue = xQueueCreate(
        16,
        sizeof(uart_reply_t)
    );

    employee_decision_queue = xQueueCreate(
        8,
        sizeof(employee_decision_t)
    );

    control_queue = xQueueCreate(
        8,
        sizeof(control_command_t)
    );

    face_uart_payload = heap_caps_malloc(
        FL_MAX_PAYLOAD,
        MALLOC_CAP_8BIT
    );

    server_response_buffer = heap_caps_malloc(
        SERVER_RESPONSE_MAX,
        MALLOC_CAP_8BIT
    );

    if (
        !uart_tx_mutex ||
        !uart_reply_queue ||
        !employee_decision_queue ||
        !control_queue ||
        !face_uart_payload ||
        !server_response_buffer
    ) {

        ESP_LOGE(
            TAG,
            "Queue or memory allocation failed"
        );

        abort();
    }

    /* --------------------------------------------------------
     * UART receiver task
     * -------------------------------------------------------- */

    if (
        xTaskCreate(
            uart_receiver_task,
            "uart_receiver",
            4096,
            NULL,
            5,
            NULL
        ) != pdPASS
    ) {

        ESP_LOGE(
            TAG,
            "UART receiver task creation failed"
        );

        abort();
    }

    /* --------------------------------------------------------
     * Camera
     * -------------------------------------------------------- */

    ESP_ERROR_CHECK(
        camera_init()
    );

    /* --------------------------------------------------------
     * Wi-Fi
     * -------------------------------------------------------- */

    wifi_init_sta();

    ESP_LOGI(
        TAG,
        "Camera initialized"
    );

    ESP_LOGI(
        TAG,
        "AI server: %s",
        FACE_SERVER_URL
    );

    /* --------------------------------------------------------
     * Initial UART synchronization
     * -------------------------------------------------------- */

    uart_send_status("CAM_READY");

    int64_t last_fallback_us = 0;

    /* --------------------------------------------------------
     * Main loop
     * -------------------------------------------------------- */

    while (true) {

        /* Process a MAIN control request. */

        control_command_t command;

        if (
            xQueueReceive(
                control_queue,
                &command,
                0
            ) == pdTRUE
        ) {

            process_control_command(command);
        }

        /* Website registration must not depend on radar. */

        poll_website_capture_mode();

        /* Decide whether to capture another frame. */

        int64_t now =
            esp_timer_get_time();

        bool fallback_scan =
            !person_present &&
            !website_enrollment_active &&
            now - last_fallback_us >=
                1500000LL;

        if (
            person_present ||
            website_enrollment_active ||
            fallback_scan
        ) {

            if (fallback_scan)
                last_fallback_us = now;

            camera_fb_t *frame =
                esp_camera_fb_get();

            if (frame) {

                send_camera_frame(frame);

                esp_camera_fb_return(frame);

            } else {

                ESP_LOGW(
                    TAG,
                    "Camera frame unavailable"
                );
            }
        }

        /* Website -> S3 -> MAIN verification. */

        submit_employee_decision();

        poll_website_registration();

        /* Server face roster -> MAIN. */

        synchronize_face_roster();

        vTaskDelay(
            pdMS_TO_TICKS(
                FACE_FRAME_PERIOD_MS
            )
        );
    }
}
