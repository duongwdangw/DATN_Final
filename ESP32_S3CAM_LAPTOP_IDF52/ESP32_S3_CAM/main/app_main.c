
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
#include "driver/gpio.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "sdkconfig.h"

#include "face_config.h"
#include "face_uart_link.h"

/* =========================================================
 * ESP32-S3-CAM
 * ESP-IDF 5.2.7
 *
 * UART1:
 *   TX GPIO43 -> ESP32 MAIN GPIO32
 *   RX GPIO44 <- ESP32 MAIN GPIO33
 *
 * Console: USB Serial/JTAG
 * ========================================================= */

#ifndef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "Set S3 console to USB Serial/JTAG in menuconfig"
#endif

#define S3_UART           UART_NUM_1
#define S3_UART_TX        GPIO_NUM_43
#define S3_UART_RX        GPIO_NUM_44
#define S3_UART_BAUD      115200

/* Server response: FCR1 + metadata + ID + RGB565 */
#define HTTP_MAGIC        "FCR1"
#define HTTP_HEADER_LEN   15U
#define MAX_EMP_ID_DIGITS 10U
#define HTTP_MATCH        1U
#define HTTP_UNKNOWN      2U

#define MAX_HTTP_REPLY \
    (HTTP_HEADER_LEN + MAX_EMP_ID_DIGITS + FACE_THUMB_BYTES)

_Static_assert(
    FACE_THUMB_BYTES == FL_MAX_IMAGE_BYTES,
    "Camera thumbnail must be RGB565 96x96"
);

static const char *TAG = "FACE_S3";

/* =========================================================
 * GLOBAL STATE
 * ========================================================= */

static SemaphoreHandle_t uart_tx_lock;
static QueueHandle_t uart_reply_queue;
static QueueHandle_t command_queue;

static uint8_t *http_reply;
static uint8_t *face_payload;

static uint32_t outgoing_seq = 1;
static bool link_ready = false;

static volatile bool has_presence = true;
static volatile bool wifi_ready = false;

static bool enrolling = false;
static uint16_t enrolling_id = 0;
static int enroll_progress = -1;
static unsigned enroll_errors = 0;

static int64_t last_enroll_poll_us = 0;
static unsigned unknown_count = 0;
static int64_t last_unknown_us = 0;

typedef struct {
    uint32_t type;
    uint32_t seq;
    uint32_t crc;
} uart_reply_t;

typedef enum {
    CMD_START = 1,
    CMD_CANCEL = 2,
    CMD_DELETE = 3
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    uint16_t id;
} face_command_t;

/* =========================================================
 * LITTLE-ENDIAN HELPERS
 * ========================================================= */

static uint16_t u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] |
           ((uint16_t)p[1] << 8);
}

static uint32_t u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* =========================================================
 * UART TX
 *
 * Serialize complete frames, including binary payload.
 * ========================================================= */

static bool uart_send(
    uint32_t type,
    uint32_t seq,
    const void *data,
    uint32_t length,
    uint32_t crc)
{
    if (xSemaphoreTake(
            uart_tx_lock,
            pdMS_TO_TICKS(6000)) != pdTRUE) {

        ESP_LOGE(TAG, "UART TX mutex timeout");
        return false;
    }

    bool ok = fl_send_frame(
        S3_UART,
        NULL,
        type,
        seq,
        data,
        length,
        crc
    );

    xSemaphoreGive(uart_tx_lock);

    return ok;
}

/* =========================================================
 * UART WAIT ACK / READY
 * ========================================================= */

static bool wait_reply(
    uint32_t expected_type,
    uint32_t expected_seq,
    uint32_t expected_crc,
    uint32_t timeout_ms)
{
    int64_t deadline =
        esp_timer_get_time() +
        (int64_t)timeout_ms * 1000;

    uart_reply_t r;

    while (esp_timer_get_time() < deadline) {

        if (xQueueReceive(
                uart_reply_queue,
                &r,
                pdMS_TO_TICKS(30)) != pdTRUE) {
            continue;
        }

        if (r.seq != expected_seq)
            continue;

        if (r.type == FL_TYPE_NACK)
            return false;

        if (r.type == expected_type &&
            r.crc == expected_crc) {
            return true;
        }
    }

    return false;
}

/* =========================================================
 * UART HANDSHAKE
 *
 * S3 -> HELLO
 * MAIN -> READY
 * ========================================================= */

static bool handshake(void)
{
    if (link_ready)
        return true;

    for (int attempt = 1; attempt <= 3; ++attempt) {

        xQueueReset(uart_reply_queue);

        ESP_LOGI(
            TAG,
            "UART HELLO attempt=%d",
            attempt
        );

        bool sent = uart_send(
            FL_TYPE_HELLO,
            0,
            NULL,
            0,
            0
        );

        if (sent &&
            wait_reply(
                FL_TYPE_READY,
                0,
                0,
                2500)) {

            link_ready = true;
            outgoing_seq = 1;

            ESP_LOGI(
                TAG,
                "UART HANDSHAKE PASS"
            );

            return true;
        }

        ESP_LOGW(
            TAG,
            "UART HELLO no READY attempt=%d",
            attempt
        );

        vTaskDelay(pdMS_TO_TICKS(350));
    }

    return false;
}

/* =========================================================
 * UART EVENT WITH CRC32 + ACK + RETRY
 * ========================================================= */

static bool send_event(
    uint32_t type,
    const uint8_t *data,
    uint32_t length)
{
    if (!data ||
        length == 0 ||
        length > FL_MAX_PAYLOAD) {
        return false;
    }

    if (!handshake()) {
        ESP_LOGW(
            TAG,
            "UART link offline; event type=%" PRIu32,
            type
        );
        return false;
    }

    uint32_t seq = outgoing_seq++;
    uint32_t crc = fl_crc32(data, length);

    for (int attempt = 1; attempt <= 3; ++attempt) {

        xQueueReset(uart_reply_queue);

        bool sent = uart_send(
            type,
            seq,
            data,
            length,
            crc
        );

        if (sent &&
            wait_reply(
                FL_TYPE_ACK,
                seq,
                crc,
                4500)) {

            if (attempt > 1) {
                ESP_LOGW(
                    TAG,
                    "UART recovered seq=%" PRIu32
                    " attempt=%d",
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
            " attempt=%d",
            type,
            seq,
            attempt
        );

        vTaskDelay(pdMS_TO_TICKS(150));
    }

    link_ready = false;

    ESP_LOGE(
        TAG,
        "UART EVENT FAILED type=%" PRIu32
        " seq=%" PRIu32,
        type,
        seq
    );

    return false;
}

static bool send_status(const char *text)
{
    size_t len = strlen(text);

    if (len == 0 || len > 110)
        return false;

    return send_event(
        FL_TYPE_STATUS,
        (const uint8_t *)text,
        (uint32_t)len
    );
}

/* =========================================================
 * UART RX TASK
 *
 * Exactly one task reads UART1.
 * Handles READY, ACK, NACK and MAIN commands.
 * ========================================================= */

static void uart_rx_task(void *arg)
{
    (void)arg;

    uint8_t payload[128];
    fl_header_t h;

    uint32_t last_cmd_seq = 0;
    uint32_t last_cmd_crc = 0;

    while (true) {

        fl_rx_result_t result = fl_read_frame(
            S3_UART,
            &h,
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
                "UART RX error=%d type=%" PRIu32
                " seq=%" PRIu32
                " len=%" PRIu32,
                (int)result,
                h.type,
                h.seq,
                h.len
            );

            if (h.type == FL_TYPE_COMMAND &&
                h.seq != 0) {

                (void)uart_send(
                    FL_TYPE_NACK,
                    h.seq,
                    NULL,
                    0,
                    0
                );
            }

            if (result == FL_RX_BAD_HEADER ||
                result == FL_RX_INCOMPLETE) {
                uart_flush_input(S3_UART);
            }

            continue;
        }

        /* Handle control responses */

        if (h.type == FL_TYPE_READY ||
            h.type == FL_TYPE_ACK ||
            h.type == FL_TYPE_NACK) {

            uart_reply_t reply = {
                h.type,
                h.seq,
                h.crc
            };

            if (h.type == FL_TYPE_READY) {
                ESP_LOGI(
                    TAG,
                    "UART RX READY from MAIN"
                );
            }

            if (xQueueSend(
                    uart_reply_queue,
                    &reply,
                    0) != pdTRUE) {

                ESP_LOGW(
                    TAG,
                    "UART reply queue full"
                );
            }

            continue;
        }

        if (h.type != FL_TYPE_COMMAND)
            continue;

        if (h.len == 0 ||
            h.len >= sizeof(payload)) {

            (void)uart_send(
                FL_TYPE_NACK,
                h.seq,
                NULL,
                0,
                0
            );

            continue;
        }

        payload[h.len] = '\0';

        const char *command =
            (const char *)payload;

        bool duplicate =
            h.seq == last_cmd_seq &&
            h.crc == last_cmd_crc;

        bool accepted = false;

        /* MAIN -> S3: presence */

        if (duplicate) {

            accepted = true;

        } else if (
            strcmp(command, "PRESENCE:1") == 0) {

            has_presence = true;
            accepted = true;

        } else if (
            strcmp(command, "PRESENCE:0") == 0) {

            has_presence = false;
            accepted = true;

        } else {

            face_command_t item = {0};
            const char *number = NULL;

            if (strncmp(
                    command,
                    "ENROLL_START:",
                    13) == 0) {

                item.kind = CMD_START;
                number = command + 13;

            } else if (strncmp(
                    command,
                    "ENROLL_CANCEL:",
                    14) == 0) {

                item.kind = CMD_CANCEL;
                number = command + 14;

            } else if (strncmp(
                    command,
                    "FACE_DELETE:",
                    12) == 0) {

                item.kind = CMD_DELETE;
                number = command + 12;
            }

            if (number &&
                *number &&
                strlen(number) <= 5) {

                unsigned id = 0;
                bool digits = true;

                for (const char *p = number;
                     *p;
                     ++p) {

                    if (*p < '0' ||
                        *p > '9') {

                        digits = false;
                        break;
                    }

                    id =
                        id * 10U +
                        (unsigned)(*p - '0');
                }

                if (digits &&
                    id > 0 &&
                    id <= 65535) {

                    item.id = (uint16_t)id;

                    accepted = xQueueSend(
                        command_queue,
                        &item,
                        0
                    ) == pdTRUE;
                }
            }
        }

        if (accepted) {

            last_cmd_seq = h.seq;
            last_cmd_crc = h.crc;

            bool ack_ok = uart_send(
                FL_TYPE_ACK,
                h.seq,
                NULL,
                0,
                h.crc
            );

            ESP_LOGI(
                TAG,
                "UART CMD %s seq=%" PRIu32
                " ACK=%d duplicate=%d",
                command,
                h.seq,
                ack_ok,
                duplicate
            );

        } else {

            ESP_LOGW(
                TAG,
                "UART command rejected: %s",
                command
            );

            (void)uart_send(
                FL_TYPE_NACK,
                h.seq,
                NULL,
                0,
                0
            );
        }
    }
}

/* =========================================================
 * SEND FACE RESULT TO ESP32 MAIN
 *
 * Payload:
 *   employee_id uint16 little-endian
 *   RGB565 image 18432 bytes
 *
 * Total: 18434 bytes
 * ========================================================= */

static void send_face_result(
    const char *id,
    const uint8_t *pixels,
    size_t n)
{
    if (!id ||
        !pixels ||
        n != FL_MAX_IMAGE_BYTES ||
        !face_payload) {
        return;
    }

    char *end = NULL;
    unsigned long employee =
        strtoul(id, &end, 10);

    if (end == id ||
        *end != '\0' ||
        employee == 0 ||
        employee > 65535) {

        ESP_LOGW(
            TAG,
            "Invalid employee ID from server"
        );

        return;
    }

    face_payload[0] =
        (uint8_t)employee;

    face_payload[1] =
        (uint8_t)(employee >> 8);

    memcpy(
        face_payload + 2,
        pixels,
        n
    );

    bool ok = send_event(
        FL_TYPE_FACE,
        face_payload,
        (uint32_t)n + 2U
    );

    if (ok) {

        ESP_LOGI(
            TAG,
            "FACE UART ACK id=%lu bytes=%u",
            employee,
            (unsigned)n
        );

    } else {

        ESP_LOGE(
            TAG,
            "FACE UART NOT ACKED id=%lu",
            employee
        );
    }
}

/* =========================================================
 * STATUS EVENTS
 * ========================================================= */

static void send_enrollment_status(
    const char *kind,
    uint16_t id,
    int count)
{
    char text[72];

    int n = count >= 0
        ? snprintf(
            text,
            sizeof(text),
            "%s:%u:%d",
            kind,
            (unsigned)id,
            count)
        : snprintf(
            text,
            sizeof(text),
            "%s:%u",
            kind,
            (unsigned)id);

    if (n > 0 &&
        n < (int)sizeof(text) &&
        !send_status(text)) {

        ESP_LOGW(
            TAG,
            "Status not ACKed: %s",
            text
        );
    }
}

/* =========================================================
 * HTTP COMMAND TO PYTHON SERVER
 * ========================================================= */

static bool http_command(
    const char *route,
    const char *body,
    char *answer,
    size_t capacity)
{
    if (!wifi_ready)
        return false;

    char url[192];

    int n = snprintf(
        url,
        sizeof(url),
        "%s%s",
        FACE_SERVER_BASE_URL,
        route
    );

    if (n <= 0 || n >= (int)sizeof(url))
        return false;

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

    int body_len =
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

    bool ok = false;

    if (esp_http_client_open(
            client,
            body_len) != ESP_OK) {
        goto done;
    }

    if (body &&
        esp_http_client_write(
            client,
            body,
            body_len) != body_len) {
        goto done;
    }

    if (esp_http_client_fetch_headers(
            client) < 0) {
        goto done;
    }

    if (esp_http_client_get_status_code(
            client) != 200) {
        goto done;
    }

    if (!answer || capacity < 2)
        goto done;

    size_t used = 0;

    while (used < capacity - 1) {

        int got = esp_http_client_read(
            client,
            answer + used,
            (int)(capacity - 1 - used)
        );

        if (got <= 0)
            break;

        used += (size_t)got;
    }

    answer[used] = '\0';
    ok = used > 0;

done:

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    return ok;
}

/* =========================================================
 * PROCESS MAIN COMMANDS
 * ========================================================= */

static void process_command(
    face_command_t command)
{
    char body[40];
    char answer[96];

    snprintf(
        body,
        sizeof(body),
        "{\"id\":\"%u\"}",
        (unsigned)command.id
    );

    if (command.kind == CMD_START) {

        if (enrolling) {

            send_enrollment_status(
                "ENROLL_ERROR",
                command.id,
                -1
            );

            return;
        }

        bool ok = http_command(
            "/api/device/enroll/start",
            body,
            answer,
            sizeof(answer)
        );

        if (ok &&
            strncmp(answer, "STARTED:", 8) == 0) {

            enrolling = true;
            enrolling_id = command.id;
            enroll_progress = -1;
            enroll_errors = 0;
            last_enroll_poll_us = 0;

            send_enrollment_status(
                "ENROLL_STARTED",
                command.id,
                -1
            );

        } else {

            send_enrollment_status(
                "ENROLL_ERROR",
                command.id,
                -1
            );
        }

    } else if (command.kind == CMD_CANCEL) {

        (void)http_command(
            "/api/device/enroll/cancel",
            body,
            answer,
            sizeof(answer)
        );

        if (enrolling_id == command.id) {

            enrolling = false;
            enrolling_id = 0;
        }

    } else if (command.kind == CMD_DELETE) {

        bool ok = http_command(
            "/api/device/face/delete",
            body,
            answer,
            sizeof(answer)
        );

        if (ok &&
            strncmp(answer, "DELETED:", 8) == 0) {

            send_enrollment_status(
                "FACE_DELETED",
                command.id,
                -1
            );
        }
    }
}

/* =========================================================
 * ENROLLMENT PROGRESS
 * ========================================================= */

static void poll_enrollment(void)
{
    if (!enrolling)
        return;

    int64_t now = esp_timer_get_time();

    if (now - last_enroll_poll_us <
        1200000LL) {
        return;
    }

    last_enroll_poll_us = now;

    char answer[96];

    bool ok = http_command(
        "/api/device/enroll/state",
        NULL,
        answer,
        sizeof(answer)
    );

    if (!ok) {

        enroll_errors++;

        if (enroll_errors >= 4) {

            send_enrollment_status(
                "ENROLL_ERROR",
                enrolling_id,
                -1
            );

            enrolling = false;
            enrolling_id = 0;
        }

        return;
    }

    enroll_errors = 0;

    unsigned id = 0;
    unsigned count = 0;

    if (sscanf(
            answer,
            "ENROLLING:%u:%u",
            &id,
            &count) == 2 &&
        id == enrolling_id) {

        if ((int)count != enroll_progress) {

            enroll_progress = (int)count;

            send_enrollment_status(
                "ENROLL_PROGRESS",
                enrolling_id,
                (int)count
            );
        }

    } else if (sscanf(
                   answer,
                   "DONE:%u",
                   &id) == 1 &&
               id == enrolling_id) {

        send_enrollment_status(
            "ENROLL_DONE",
            enrolling_id,
            -1
        );

        enrolling = false;
        enrolling_id = 0;

    } else {

        send_enrollment_status(
            "ENROLL_ERROR",
            enrolling_id,
            -1
        );

        enrolling = false;
        enrolling_id = 0;
    }
}

/* =========================================================
 * WIFI EVENTS
 * ========================================================= */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_ready = false;

        ESP_LOGW(
            TAG,
            "Wi-Fi disconnected"
        );

        esp_wifi_connect();

    } else if (base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *ip =
            (ip_event_got_ip_t *)event_data;

        wifi_ready = true;

        ESP_LOGI(
            TAG,
            "Wi-Fi IP: " IPSTR,
            IP2STR(&ip->ip_info.ip)
        );
    }
}

/* =========================================================
 * WIFI INITIALIZATION
 * ========================================================= */

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t config =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&config)
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

    wifi_config_t credentials = {0};

    snprintf(
        (char *)credentials.sta.ssid,
        sizeof(credentials.sta.ssid),
        "%s",
        FACE_WIFI_SSID
    );

    snprintf(
        (char *)credentials.sta.password,
        sizeof(credentials.sta.password),
        "%s",
        FACE_WIFI_PASSWORD
    );

    credentials.sta.threshold.authmode =
        WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &credentials
        )
    );

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(
        esp_wifi_set_ps(WIFI_PS_NONE)
    );

    esp_wifi_connect();
}

/* =========================================================
 * CAMERA INITIALIZATION
 * ========================================================= */

static esp_err_t init_camera(void)
{
    bool psram =
        heap_caps_get_total_size(
            MALLOC_CAP_SPIRAM) > 0;

    ESP_LOGI(
        TAG,
        "PSRAM=%u bytes",
        (unsigned)heap_caps_get_total_size(
            MALLOC_CAP_SPIRAM)
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

        .frame_size = psram
            ? FRAMESIZE_VGA
            : FRAMESIZE_QVGA,

        .jpeg_quality = psram ? 10 : 13,
        .fb_count = psram ? 2 : 1,

        .fb_location = psram
            ? CAMERA_FB_IN_PSRAM
            : CAMERA_FB_IN_DRAM,

        .grab_mode = psram
            ? CAMERA_GRAB_LATEST
            : CAMERA_GRAB_WHEN_EMPTY
    };

    esp_err_t result =
        esp_camera_init(&config);

    if (result == ESP_OK) {

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
    }

    return result;
}

/* =========================================================
 * UART INITIALIZATION
 * ========================================================= */

static void init_uart(void)
{
    uart_config_t config = {

        .baud_rate = S3_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(
        uart_driver_install(
            S3_UART,
            4096,
            4096,
            0,
            NULL,
            0
        )
    );

    ESP_ERROR_CHECK(
        uart_param_config(
            S3_UART,
            &config
        )
    );

    ESP_ERROR_CHECK(
        uart_set_pin(
            S3_UART,
            S3_UART_TX,
            S3_UART_RX,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    ESP_LOGI(
        TAG,
        "UART1 TX=GPIO43 RX=GPIO44 BAUD=115200"
    );
}

/* =========================================================
 * HTTP BINARY READ
 * ========================================================= */

static bool http_read_n(
    esp_http_client_handle_t client,
    uint8_t *dest,
    size_t n)
{
    size_t done = 0;

    while (done < n) {

        int got = esp_http_client_read(
            client,
            (char *)dest + done,
            (int)(n - done)
        );

        if (got <= 0)
            return false;

        done += (size_t)got;
    }

    return true;
}

/* =========================================================
 * CAMERA -> PYTHON SERVER
 *
 * Response:
 * FCR1 + status + id_len + width + height + image_len
 * + employee ID + RGB565 96x96
 * ========================================================= */

static void upload_frame(camera_fb_t *frame)
{
    if (!wifi_ready)
        return;

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

    if (esp_http_client_open(
            client,
            (int)frame->len) != ESP_OK) {
        goto done;
    }

    size_t written = 0;

    while (written < frame->len) {

        int n = esp_http_client_write(
            client,
            (const char *)frame->buf + written,
            (int)(frame->len - written)
        );

        if (n <= 0)
            goto done;

        written += (size_t)n;
    }

    if (esp_http_client_fetch_headers(
            client) < 0) {
        goto done;
    }

    if (esp_http_client_get_status_code(
            client) != 200) {
        goto done;
    }

    if (!http_read_n(
            client,
            http_reply,
            HTTP_HEADER_LEN)) {
        goto done;
    }

    if (memcmp(
            http_reply,
            HTTP_MAGIC,
            4) != 0) {
        goto done;
    }

    uint8_t status = http_reply[4];

    uint32_t id_len =
        u16_le(http_reply + 5);

    uint32_t width =
        u16_le(http_reply + 7);

    uint32_t height =
        u16_le(http_reply + 9);

    uint32_t image_len =
        u32_le(http_reply + 11);

    if (status == HTTP_UNKNOWN &&
        !enrolling) {

        unknown_count++;

        if (unknown_count >= 3 &&
            esp_timer_get_time() -
                last_unknown_us >= 5000000LL) {

            (void)send_status(
                "FACE_UNKNOWN"
            );

            unknown_count = 0;

            last_unknown_us =
                esp_timer_get_time();
        }

    } else {
        unknown_count = 0;
    }

    if (status == HTTP_MATCH &&
        id_len > 0 &&
        id_len <= MAX_EMP_ID_DIGITS &&
        width == FACE_THUMB_W &&
        height == FACE_THUMB_H &&
        image_len == FACE_THUMB_BYTES) {

        if (!http_read_n(
                client,
                http_reply + HTTP_HEADER_LEN,
                id_len + image_len)) {
            goto done;
        }

        char id[MAX_EMP_ID_DIGITS + 1];

        memcpy(
            id,
            http_reply + HTTP_HEADER_LEN,
            id_len
        );

        id[id_len] = '\0';

        bool valid_id = true;

        for (uint32_t i = 0; i < id_len; ++i) {

            if (id[i] < '0' ||
                id[i] > '9') {

                valid_id = false;
                break;
            }
        }

        if (valid_id) {

            send_face_result(
                id,
                http_reply +
                    HTTP_HEADER_LEN + id_len,
                image_len
            );
        }
    }

done:

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* =========================================================
 * APP MAIN
 * ========================================================= */

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();

    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        nvs = nvs_flash_init();
    }

    ESP_ERROR_CHECK(nvs);

    init_uart();

    uart_tx_lock =
        xSemaphoreCreateMutex();

    uart_reply_queue =
        xQueueCreate(
            16,
            sizeof(uart_reply_t)
        );

    command_queue =
        xQueueCreate(
            8,
            sizeof(face_command_t)
        );

    face_payload = heap_caps_malloc(
        FL_MAX_PAYLOAD,
        MALLOC_CAP_8BIT
    );

    if (!uart_tx_lock ||
        !uart_reply_queue ||
        !command_queue ||
        !face_payload) {

        ESP_LOGE(
            TAG,
            "UART allocation failed"
        );

        abort();
    }

    if (xTaskCreate(
            uart_rx_task,
            "s3_uart_rx",
            4096,
            NULL,
            6,
            NULL) != pdPASS) {

        abort();
    }

    ESP_ERROR_CHECK(
        init_camera()
    );

    http_reply = heap_caps_malloc(
        MAX_HTTP_REPLY,
        MALLOC_CAP_8BIT
    );

    if (!http_reply) {

        ESP_LOGE(
            TAG,
            "HTTP reply allocation failed"
        );

        abort();
    }

    init_wifi();

    ESP_LOGI(
        TAG,
        "Camera initialized, server=%s",
        FACE_SERVER_URL
    );

    /* Announce camera readiness to MAIN */

    (void)send_status("CAM_READY");

    int64_t last_fallback_scan = 0;

    while (true) {

        face_command_t command;

        if (xQueueReceive(
                command_queue,
                &command,
                0) == pdTRUE) {

            process_command(command);
        }

        int64_t now =
            esp_timer_get_time();

        bool fallback_scan =
            !has_presence &&
            !enrolling &&
            now - last_fallback_scan >=
                1500000LL;

        if (wifi_ready &&
            (has_presence ||
             enrolling ||
             fallback_scan)) {

            if (fallback_scan)
                last_fallback_scan = now;

            camera_fb_t *fb =
                esp_camera_fb_get();

            if (fb) {

                upload_frame(fb);
                esp_camera_fb_return(fb);

            } else {

                ESP_LOGW(
                    TAG,
                    "camera frame capture failed"
                );
            }
        }

        poll_enrollment();

        vTaskDelay(
            pdMS_TO_TICKS(
                FACE_FRAME_PERIOD_MS
            )
        );
    }
}
