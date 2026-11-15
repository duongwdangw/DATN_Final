
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "nvs_flash.h"
#include "wifi_time.h"
#include "google_sheet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <ctype.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "face_uart_link.h"
#include "nvs.h"

static const char *TAG = "ACCESS";

/* ============================================================
 * BUZZER
 *
 * Disabled until a safe transistor/MOSFET driver is installed.
 * A buzzer connected directly between 3V3 and GND cannot be
 * switched by software.
 * ============================================================ */

#define BUZZER_ENABLE 0
#define BUZZER_PIN GPIO_NUM_5

#if BUZZER_ENABLE

static QueueHandle_t buzzer_queue;

static void buzzer_task(void *arg)
{
    (void)arg;

    uint8_t event;

    while (true) {
        if (xQueueReceive(
            buzzer_queue,
            &event,
            portMAX_DELAY) == pdTRUE) {

            gpio_set_level(BUZZER_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(80));
            gpio_set_level(BUZZER_PIN, 0);
        }
    }
}

static void buzzer_init(void)
{
    gpio_reset_pin(BUZZER_PIN);
    gpio_set_level(BUZZER_PIN, 0);

    ESP_ERROR_CHECK(
        gpio_set_direction(BUZZER_PIN, GPIO_MODE_OUTPUT)
    );

    buzzer_queue = xQueueCreate(2, sizeof(uint8_t));

    if (!buzzer_queue ||
        xTaskCreate(
            buzzer_task,
            "buzzer",
            2048,
            NULL,
            3,
            NULL
        ) != pdPASS) {

        ESP_LOGE(TAG, "BUZZER init failed");
    }
}

static void buzzer_notify_success(void)
{
    if (buzzer_queue) {
        uint8_t event = 1;
        (void)xQueueSend(buzzer_queue, &event, 0);
    }
}

#else

static inline void buzzer_init(void) {}
static inline void buzzer_notify_success(void) {}

#endif

/* ============================================================
 * PIN CONFIGURATION
 * ============================================================ */

#define RC522_HOST       SPI2_HOST

#define PIN_SCK          18
#define PIN_MOSI         23
#define PIN_MISO         19
#define PIN_RC522_CS     21

#define PIN_AS608_RX     16
#define PIN_AS608_TX     17

#define PIN_CAM_RX       32
#define PIN_CAM_TX       33

#define PIN_LD2410_OUT   35

#define PIN_TFT_CS       15
#define PIN_TFT_DC       2

static const gpio_num_t rows[] = {
    4, 12, 13, 14
};

static const gpio_num_t cols[] = {
    22, 25, 26, 27
};

#define AS608_UART       UART_NUM_2
#define CAM_UART         UART_NUM_1

#define AS608_BAUD       57600
#define CAM_BAUD         115200

#define TFT_W            128
#define TFT_H            160

#define RFID_UID_LEN     4

#define AS608_OK         0x00
#define AS608_NO_FINGER  0x02
#define AS608_NO_MATCH   0x09

#define ENROLL_TIMEOUT_US \
    (60LL * 1000000LL)

#define FACE_ENROLL_TIMEOUT_US \
    (180LL * 1000000LL)

#define ADMIN_IDLE_TIMEOUT_US \
    (30LL * 1000000LL)

#define ATTENDANCE_RESULT_US \
    (1500000LL)

#define RESULT_GREEN 0x07E0
#define RESULT_RED   0xF800

/* ============================================================
 * SYSTEM STATE
 * ============================================================ */

static spi_device_handle_t rc522;
static esp_lcd_panel_io_handle_t tft_io;

static uint16_t tft_frame[TFT_W * TFT_H];

static int64_t last_accept_us;
static int64_t result_until_us;

static bool result_active;
static bool rfid_need_release;
static bool fp_need_release;

static SemaphoreHandle_t tft_mutex;
static SemaphoreHandle_t spi_bus_mutex;
static SemaphoreHandle_t as608_mutex;
static SemaphoreHandle_t cam_tx_mutex;
static SemaphoreHandle_t state_mutex;

typedef enum {
    ENROLL_NONE = 0,
    ENROLL_WAIT_RFID,
    ENROLL_WAIT_FINGER_1,
    ENROLL_WAIT_FINGER_REMOVE,
    ENROLL_WAIT_FINGER_2,
    ENROLL_WAIT_RETRY_REMOVE,
    ENROLL_WAIT_VERIFY_REMOVE,
    ENROLL_WAIT_VERIFY,
    ENROLL_WAIT_FACE
} enroll_state_t;

static enroll_state_t current_enroll_state = ENROLL_NONE;

static bool admin_authenticated;
static bool admin_pin_entry_active;

static uint16_t enroll_emp_id;
static uint8_t enroll_rfid_uid[RFID_UID_LEN];
static bool enroll_rfid_pending;

static int64_t enroll_started_us;

static uint16_t fp_library_size = 300;

static char input_buffer[12];
static size_t input_len;

#define LOCK() \
    xSemaphoreTake(state_mutex, portMAX_DELAY)

#define UNLOCK() \
    xSemaphoreGive(state_mutex)

typedef enum {
    ADMIN_UI_NONE = 0,
    ADMIN_UI_MENU,
    ADMIN_UI_ENTER_ADD_ID,
    ADMIN_UI_ENTER_DEL_ID,
    ADMIN_UI_ENTER_FACE_ID
} admin_ui_mode_t;

/* ============================================================
 * TFT DISPLAY
 * ============================================================ */

static void tft_cmd(
    uint8_t cmd,
    const uint8_t *param,
    size_t len)
{
    ESP_ERROR_CHECK(
        esp_lcd_panel_io_tx_param(
            tft_io,
            cmd,
            param,
            len
        )
    );
}

static void tft_flush(void)
{
    uint8_t x[] = {
        0, 0, 0, TFT_W - 1
    };

    uint8_t y[] = {
        0, 0, 0, TFT_H - 1
    };

    tft_cmd(0x2A, x, sizeof(x));
    tft_cmd(0x2B, y, sizeof(y));

    ESP_ERROR_CHECK(
        esp_lcd_panel_io_tx_color(
            tft_io,
            0x2C,
            tft_frame,
            sizeof(tft_frame)
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_io_tx_param(
            tft_io,
            0x00,
            NULL,
            0
        )
    );
}

/* ST7735 uses MSB-first RGB565 bytes. */
static inline uint16_t tft_wire_color(uint16_t rgb565)
{
    return (uint16_t)(
        (rgb565 << 8) |
        (rgb565 >> 8)
    );
}

static void tft_fill(uint16_t color)
{
    const uint16_t wire_color =
        tft_wire_color(color);

    for (size_t i = 0; i < TFT_W * TFT_H; ++i) {
        tft_frame[i] = wire_color;
    }
}

static void tft_box(
    int x,
    int y,
    int w,
    int h,
    uint16_t color)
{
    const uint16_t wire_color =
        tft_wire_color(color);

    for (int yy = y; yy < y + h && yy < TFT_H; ++yy) {
        for (int xx = x; xx < x + w && xx < TFT_W; ++xx) {
            if (xx >= 0 && yy >= 0) {
                tft_frame[yy * TFT_W + xx] =
                    wire_color;
            }
        }
    }
}

/* 5x7 ASCII font */
static const uint8_t glyph[][5] = {
    {0,0,0,0,0},
    {0x1e,0x05,0x05,0x1e,0},
    {0x1f,0x15,0x15,0x0a,0},
    {0x0e,0x11,0x11,0x11,0},
    {0x1f,0x11,0x11,0x0e,0},
    {0x1f,0x15,0x15,0x11,0},
    {0x1f,0x05,0x05,0x01,0},
    {0x0e,0x11,0x15,0x1d,0},
    {0x1f,0x04,0x04,0x1f,0},
    {0x11,0x1f,0x11,0,0},
    {0x08,0x10,0x10,0x0f,0},
    {0x1f,0x04,0x0a,0x11,0},
    {0x1f,0x10,0x10,0x10,0},
    {0x1f,0x02,0x04,0x02,0x1f},
    {0x1f,0x02,0x04,0x1f,0},
    {0x0e,0x11,0x11,0x0e,0},
    {0x1f,0x05,0x05,0x02,0},
    {0x0e,0x11,0x19,0x1e,0},
    {0x1f,0x05,0x0d,0x12,0},
    {0x12,0x15,0x15,0x09,0},
    {0x01,0x1f,0x01,0,0},
    {0x0f,0x10,0x10,0x0f,0},
    {0x07,0x08,0x10,0x08,0x07},
    {0x1f,0x08,0x04,0x08,0x1f},
    {0x1b,0x04,0x04,0x1b,0},
    {0x03,0x04,0x18,0x04,0x03},
    {0x19,0x15,0x13,0,0},
    {0x0e,0x11,0x11,0x0e,0},
    {0x00,0x12,0x1f,0x10,0},
    {0x19,0x15,0x15,0x12,0},
    {0x11,0x15,0x15,0x0a,0},
    {0x07,0x04,0x04,0x1f,0},
    {0x17,0x15,0x15,0x09,0},
    {0x0e,0x15,0x15,0x08,0},
    {0x01,0x01,0x1d,0x03,0},
    {0x0a,0x15,0x15,0x0a,0},
    {0x02,0x15,0x15,0x0e,0},
    {0x00,0x00,0x14,0x00,0x00},
    {0x40,0x20,0x08,0x02,0x01}
};

static int glyph_index(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return 1 + c - 'A';
    }

    if (c >= '0' && c <= '9') {
        return 27 + c - '0';
    }

    if (c == ':') return 37;
    if (c == '/') return 38;

    return 0;
}

static void tft_text(
    int x,
    int y,
    const char *text,
    uint16_t color)
{
    for (; *text; ++text, x += 6) {
        const uint8_t *g =
            glyph[glyph_index(*text)];

        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 7; ++row) {
                if (g[col] & (1U << row)) {
                    tft_box(
                        x + col,
                        y + row,
                        1,
                        1,
                        color
                    );
                }
            }
        }
    }
}

#define UI_BLACK 0x0000
#define UI_WHITE 0xFFFF

static void tft_message_color(
    const char *title,
    const char *line1,
    const char *line2,
    uint16_t color)
{
    if (!tft_io) return;

    if (tft_mutex)
        xSemaphoreTake(tft_mutex, portMAX_DELAY);

    if (spi_bus_mutex)
        xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);

    bool failed = (color == RESULT_RED);

    tft_fill(
        failed ? RESULT_RED : UI_BLACK
    );

    tft_text(
        5, 12,
        title ? title : "",
        UI_WHITE
    );

    tft_box(
        5, 28,
        TFT_W - 10,
        1,
        UI_WHITE
    );

    tft_text(
        5, 52,
        line1 ? line1 : "",
        UI_WHITE
    );

    tft_text(
        5, 74,
        line2 ? line2 : "",
        UI_WHITE
    );

    tft_flush();

    if (spi_bus_mutex)
        xSemaphoreGive(spi_bus_mutex);

    if (tft_mutex)
        xSemaphoreGive(tft_mutex);
}

static void tft_message(
    const char *title,
    const char *line1,
    const char *line2)
{
    tft_message_color(
        title, line1, line2, UI_WHITE
    );
}

static void tft_ready_screen(void)
{
    char day_str[12];
    char time_str[16];
    char date_str[16];

    wifi_time_get_display(
        day_str,
        sizeof(day_str),
        time_str,
        sizeof(time_str),
        date_str,
        sizeof(date_str)
    );

    if (!tft_io) return;

    if (tft_mutex)
        xSemaphoreTake(tft_mutex, portMAX_DELAY);

    if (spi_bus_mutex)
        xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);

    tft_fill(UI_BLACK);

    tft_text(
        5, 14,
        "SYSTEM READY",
        UI_WHITE
    );

    tft_box(
        5, 31,
        TFT_W - 10,
        1,
        UI_WHITE
    );

    tft_text(
        5, 61,
        day_str,
        UI_WHITE
    );

    tft_text(
        5, 84,
        time_str,
        UI_WHITE
    );

    tft_text(
        5, 107,
        date_str,
        UI_WHITE
    );

    tft_flush();

    if (spi_bus_mutex)
        xSemaphoreGive(spi_bus_mutex);

    if (tft_mutex)
        xSemaphoreGive(tft_mutex);
}

static void tft_result_screen(
    const char *method,
    const char *id,
    bool success)
{
    char day_str[12];
    char time_str[16];
    char date_str[16];

    wifi_time_get_display(
        day_str,
        sizeof(day_str),
        time_str,
        sizeof(time_str),
        date_str,
        sizeof(date_str)
    );

    if (!tft_io) return;

    if (tft_mutex)
        xSemaphoreTake(tft_mutex, portMAX_DELAY);

    if (spi_bus_mutex)
        xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);

    tft_fill(
        success ? RESULT_GREEN : RESULT_RED
    );

    tft_text(
        5, 14,
        success ? "SUCCESS" : "FAILED",
        UI_WHITE
    );

    tft_box(
        5, 31,
        TFT_W - 10,
        1,
        UI_WHITE
    );

    if (success) {
        char line1[24];

        snprintf(
            line1,
            sizeof(line1),
            "%s %s",
            method ? method : "",
            id ? id : ""
        );

        tft_text(
            5, 48,
            line1,
            UI_WHITE
        );

        tft_text(
            5, 69,
            "THANK YOU",
            UI_WHITE
        );
    } else {
        tft_text(
            5, 48,
            "ACCESS DENIED",
            UI_WHITE
        );

        tft_text(
            5, 69,
            "PLEASE TRY AGAIN",
            UI_WHITE
        );
    }

    tft_text(5, 96, day_str, UI_WHITE);
    tft_text(5, 115, time_str, UI_WHITE);
    tft_text(5, 134, date_str, UI_WHITE);

    tft_flush();

    if (spi_bus_mutex)
        xSemaphoreGive(spi_bus_mutex);

    if (tft_mutex)
        xSemaphoreGive(tft_mutex);
}

/* ============================================================
 * TFT INPUT SCREEN
 * ============================================================ */

static void tft_input_screen(
    const char *title,
    const char *prompt,
    const char *value,
    const char *hint1,
    const char *hint2)
{
    if (!tft_io) return;

    if (tft_mutex)
        xSemaphoreTake(tft_mutex, portMAX_DELAY);

    if (spi_bus_mutex)
        xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);

    tft_fill(UI_BLACK);

    tft_text(5, 11, title ? title : "", UI_WHITE);
    tft_box(5, 29, TFT_W - 10, 1, UI_WHITE);
    tft_text(8, 40, prompt ? prompt : "", UI_WHITE);

    tft_box(6, 60, TFT_W - 12, 1, UI_WHITE);
    tft_box(6, 79, TFT_W - 12, 1, UI_WHITE);
    tft_box(6, 60, 1, 20, UI_WHITE);
    tft_box(TFT_W - 7, 60, 1, 20, UI_WHITE);

    tft_text(
        10, 66,
        (value && *value) ? value : "-",
        UI_WHITE
    );

    tft_text(5, 104, hint1 ? hint1 : "", UI_WHITE);
    tft_text(5, 121, hint2 ? hint2 : "", UI_WHITE);

    tft_flush();

    if (spi_bus_mutex)
        xSemaphoreGive(spi_bus_mutex);

    if (tft_mutex)
        xSemaphoreGive(tft_mutex);
}

/* ============================================================
 * FACE IMAGE ON TFT
 *
 * RGB565 96x96 pixels.
 * 96 * 96 * 2 = 18432 bytes.
 * ============================================================ */

#define FACE_PHOTO_W 96
#define FACE_PHOTO_H 96

#define FACE_PHOTO_LEN \
    (FACE_PHOTO_W * FACE_PHOTO_H * 2)

static uint16_t face_photo[
    FACE_PHOTO_W * FACE_PHOTO_H
];

static uint8_t face_rx_payload[FL_MAX_PAYLOAD];

static void tft_face_photo_screen(const char *id)
{
    if (!tft_io) return;

    xSemaphoreTake(tft_mutex, portMAX_DELAY);
    xSemaphoreTake(spi_bus_mutex, portMAX_DELAY);

    tft_fill(RESULT_GREEN);
    tft_text(6, 9, "FACE SUCCESS", UI_WHITE);

    int ox = (TFT_W - FACE_PHOTO_W) / 2;
    int oy = 28;

    for (int y = 0; y < FACE_PHOTO_H; ++y) {
        for (int x = 0; x < FACE_PHOTO_W; ++x) {
            tft_frame[(oy + y) * TFT_W + ox + x] =
                tft_wire_color(
                    face_photo[y * FACE_PHOTO_W + x]
                );
        }
    }

    char label[23];

    snprintf(
        label,
        sizeof(label),
        "EMP ID: %s",
        id ? id : ""
    );

    tft_text(6, 134, label, UI_WHITE);

    tft_flush();

    xSemaphoreGive(spi_bus_mutex);
    xSemaphoreGive(tft_mutex);
}

/* ============================================================
 * TFT INITIALIZATION
 * ============================================================ */

static void tft_init(void)
{
    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = PIN_TFT_CS,
        .dc_gpio_num = PIN_TFT_DC,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)RC522_HOST,
            &io,
            &tft_io
        )
    );

    tft_cmd(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));

    tft_cmd(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t madctl = 0xC0;
    uint8_t colmod = 0x05;

    tft_cmd(0x36, &madctl, 1);
    tft_cmd(0x3A, &colmod, 1);
    tft_cmd(0x13, NULL, 0);
    tft_cmd(0x29, NULL, 0);

    tft_ready_screen();
}

/* ============================================================
 * RFID NVS STORAGE
 *
 * RFID UID is mapped to employee ID.
 * Example:
 *
 * UID A1B2C3D4 -> Employee ID 101.
 *
 * RFID, fingerprint and face use the same employee ID.
 * ============================================================ */

static void rfid_key(
    const uint8_t uid[RFID_UID_LEN],
    char key[16])
{
    snprintf(
        key,
        16,
        "u%02X%02X%02X%02X",
        uid[0],
        uid[1],
        uid[2],
        uid[3]
    );
}

static esp_err_t save_rfid_to_nvs(
    const uint8_t uid[RFID_UID_LEN],
    uint16_t emp_id)
{
    char key[16];
    rfid_key(uid, key);

    nvs_handle_t h;

    esp_err_t err = nvs_open(
        "users",
        NVS_READWRITE,
        &h
    );

    if (err != ESP_OK)
        return err;

    err = nvs_set_u16(h, key, emp_id);

    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);

    return err;
}

static esp_err_t erase_rfid_from_nvs(
    const uint8_t uid[RFID_UID_LEN])
{
    char key[16];
    rfid_key(uid, key);

    nvs_handle_t h;

    esp_err_t err = nvs_open(
        "users",
        NVS_READWRITE,
        &h
    );

    if (err != ESP_OK)
        return err;

    err = nvs_erase_key(h, key);

    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;

    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);

    return err;
}

static uint16_t check_rfid_in_nvs(
    const uint8_t uid[RFID_UID_LEN])
{
    char key[16];
    rfid_key(uid, key);

    uint16_t emp_id = 0;

    nvs_handle_t h;

    if (nvs_open(
            "users",
            NVS_READONLY,
            &h) == ESP_OK) {

        (void)nvs_get_u16(
            h,
            key,
            &emp_id
        );

        nvs_close(h);
    }

    return emp_id;
}

/* Find RFID card associated with a known employee ID. */

static bool find_uid_by_emp_id(
    uint16_t emp_id,
    uint8_t uid_out[RFID_UID_LEN])
{
    bool found = false;
    nvs_handle_t h;

    if (nvs_open(
            "users",
            NVS_READONLY,
            &h) != ESP_OK)
        return false;

    nvs_iterator_t it = NULL;

    esp_err_t res = nvs_entry_find(
        NVS_DEFAULT_PART_NAME,
        "users",
        NVS_TYPE_U16,
        &it
    );

    while (res == ESP_OK && it != NULL) {

        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        uint16_t value = 0;

        if (nvs_get_u16(
                h,
                info.key,
                &value) == ESP_OK &&
            value == emp_id) {

            unsigned int b0 = 0;
            unsigned int b1 = 0;
            unsigned int b2 = 0;
            unsigned int b3 = 0;

            if (sscanf(
                    info.key,
                    "u%02x%02x%02x%02x",
                    &b0,
                    &b1,
                    &b2,
                    &b3) == 4) {

                uid_out[0] = (uint8_t)b0;
                uid_out[1] = (uint8_t)b1;
                uid_out[2] = (uint8_t)b2;
                uid_out[3] = (uint8_t)b3;

                found = true;
            }

            break;
        }

        res = nvs_entry_next(&it);
    }

    if (it)
        nvs_release_iterator(it);

    nvs_close(h);

    return found;
}

/* ============================================================
 * FACE NVS
 *
 * Used for local face state and pending remote deletions.
 * A pending deletion prevents FACE authentication.
 * ============================================================ */

static void face_nvs_key(
    uint16_t id,
    char key[9])
{
    snprintf(
        key,
        9,
        "f%04X",
        (unsigned)id
    );
}

static bool face_nvs_get(
    const char *ns,
    uint16_t id)
{
    nvs_handle_t h;
    uint8_t value = 0;

    char key[9];
    face_nvs_key(id, key);

    if (nvs_open(
            ns,
            NVS_READONLY,
            &h) != ESP_OK)
        return false;

    esp_err_t err = nvs_get_u8(
        h,
        key,
        &value
    );

    nvs_close(h);

    return err == ESP_OK && value == 1;
}

static esp_err_t face_nvs_set(
    const char *ns,
    uint16_t id,
    bool enabled)
{
    nvs_handle_t h;

    char key[9];
    face_nvs_key(id, key);

    esp_err_t err = nvs_open(
        ns,
        NVS_READWRITE,
        &h
    );

    if (err != ESP_OK)
        return err;

    if (enabled) {
        err = nvs_set_u8(h, key, 1);
    } else {
        err = nvs_erase_key(h, key);

        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
    }

    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);

    return err;
}

/* ============================================================
 * FACE ROSTER
 *
 * Current enrolled-face IDs come from the Python server.
 *
 * MAIN receives the roster through S3-CAM UART.
 *
 * An ID must have:
 *  - a valid employee range
 *  - an RFID mapping on MAIN
 *  - no pending deletion
 *  - a fresh, complete server face roster
 *
 * Staging prevents an incomplete roster replacing the
 * last committed roster.
 * ============================================================ */

#define FACE_ROSTER_BYTES 8192U

static uint8_t face_roster[FACE_ROSTER_BYTES];
static uint8_t face_staging[FACE_ROSTER_BYTES];

static bool face_roster_ready;
static bool face_receiving;

static uint32_t face_roster_gen;

static uint32_t face_roster_expected;
static uint32_t face_roster_seen;

static int64_t face_roster_at_us;
static uint32_t face_roster_last_id;

static bool bitmap_has(
    const uint8_t *bits,
    uint16_t id)
{
    return (
        bits[id >> 3] &
        (uint8_t)(1U << (id & 7))
    ) != 0;
}

static void bitmap_put(
    uint8_t *bits,
    uint16_t id)
{
    bits[id >> 3] |=
        (uint8_t)(1U << (id & 7));
}

static void bitmap_del(
    uint8_t *bits,
    uint16_t id)
{
    bits[id >> 3] &=
        (uint8_t)~(1U << (id & 7));
}

/* A face ID is accepted only after roster synchronization. */

static bool face_employee_authorized(uint16_t id)
{
    if (id == 0 || id >= fp_library_size)
        return false;

    LOCK();

    bool ok =
        face_roster_ready &&
        esp_timer_get_time() -
            face_roster_at_us < 45000000LL &&
        bitmap_has(face_roster, id);

    UNLOCK();

    if (!ok)
        return false;

    uint8_t uid[RFID_UID_LEN];

    return
        !face_nvs_get("face_del", id) &&
        find_uid_by_emp_id(id, uid);
}

/* ============================================================
 * CAMERA UART COMMAND QUEUE
 *
 * MAIN -> S3:
 *   PRESENCE:0
 *   PRESENCE:1
 *   FACE_DELETE:<id>
 *   EMP_OK:<id>:<token>
 *   EMP_NO:<id>:<token>
 *
 * All command transmissions are serialized.
 * ============================================================ */

typedef struct {
    char line[112];
} cam_command_t;

typedef struct {
    uint32_t type;
    uint32_t seq;
    uint32_t crc;
} cam_reply_t;

static QueueHandle_t cam_command_q;
static QueueHandle_t cam_reply_q;

static uint32_t cam_command_sequence = 0;
static volatile bool cam_link_ready;

static bool cam_send_line(const char *line)
{
    if (!cam_command_q || !line)
        return false;

    size_t n = strlen(line);

    if (n == 0 ||
        n >= sizeof(((cam_command_t *)0)->line))
        return false;

    cam_command_t c = {0};

    memcpy(c.line, line, n + 1);

    return xQueueSend(
        cam_command_q,
        &c,
        pdMS_TO_TICKS(100)
    ) == pdTRUE;
}

/* Wait for the ACK corresponding to this command. */

static bool cam_wait_reply(
    uint32_t seq,
    uint32_t crc)
{
    cam_reply_t r;

    int64_t deadline =
        esp_timer_get_time() + 4500000LL;

    while (esp_timer_get_time() < deadline) {

        if (xQueueReceive(
                cam_reply_q,
                &r,
                pdMS_TO_TICKS(20)) != pdTRUE)
            continue;

        if (r.seq != seq)
            continue;

        if (r.type == FL_TYPE_NACK)
            return false;

        if (r.type == FL_TYPE_ACK &&
            r.crc == crc)
            return true;
    }

    return false;
}

/* Sends UART commands using ACK + retry. */

static void cam_command_task(void *arg)
{
    (void)arg;

    cam_command_t c;

    while (true) {

        if (xQueueReceive(
                cam_command_q,
                &c,
                portMAX_DELAY) != pdTRUE)
            continue;

        const uint32_t seq =
            ++cam_command_sequence;

        const uint32_t n =
            (uint32_t)strlen(c.line);

        const uint32_t crc =
            fl_crc32(
                (const uint8_t *)c.line,
                n
            );

        bool confirmed = false;

        for (unsigned attempt = 1;
             attempt <= 3;
             attempt++) {

            int64_t deadline =
                esp_timer_get_time() + 3000000LL;

            while (!cam_link_ready &&
                   esp_timer_get_time() < deadline) {

                vTaskDelay(
                    pdMS_TO_TICKS(50)
                );
            }

            if (!cam_link_ready)
                continue;

            xQueueReset(cam_reply_q);

            if (
                fl_send_frame(
                    CAM_UART,
                    cam_tx_mutex,
                    FL_TYPE_COMMAND,
                    seq,
                    c.line,
                    n,
                    crc
                ) &&
                cam_wait_reply(seq, crc)
            ) {

                confirmed = true;
                break;
            }

            ESP_LOGW(
                TAG,
                "CAM COMMAND retry=%u seq=%" PRIu32
                " cmd=%s",
                attempt,
                seq,
                c.line
            );
        }

        if (confirmed) {
            ESP_LOGI(
                TAG,
                "CAM COMMAND ACK seq=%" PRIu32
                " cmd=%s",
                seq,
                c.line
            );
        } else {
            ESP_LOGE(
                TAG,
                "CAM COMMAND NOT ACKED seq=%" PRIu32
                " cmd=%s",
                seq,
                c.line
            );
        }
    }
}

/* Create an ID-specific command. */

static void cam_send_command(
    const char *verb,
    uint16_t id)
{
    char line[40];

    int n = snprintf(
        line,
        sizeof(line),
        "%s:%u",
        verb,
        (unsigned)id
    );

    if (n > 0 &&
        n < (int)sizeof(line) &&
        !cam_send_line(line)) {

        ESP_LOGW(
            TAG,
            "UART command send failed: %s",
            line
        );
    }
}

/* Retry pending face deletion after network recovery. */

static void face_retry_pending_delete(void)
{
    nvs_iterator_t it = NULL;

    esp_err_t err = nvs_entry_find(
        NVS_DEFAULT_PART_NAME,
        "face_del",
        NVS_TYPE_U8,
        &it
    );

    if (err == ESP_OK && it != NULL) {

        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        unsigned id = 0;

        if (
            sscanf(
                info.key,
                "f%04X",
                &id
            ) == 1 &&
            id > 0 &&
            id < 65536
        ) {
            cam_send_command(
                "FACE_DELETE",
                (uint16_t)id
            );
        }
    }

    if (it)
        nvs_release_iterator(it);
}

/* ============================================================
 * ADMIN PIN
 * ============================================================ */

static bool nvs_pin_ok(const char *pin)
{
    char saved[12] = "1234";
    size_t n = sizeof(saved);

    nvs_handle_t h;

    if (nvs_open(
            "cfg",
            NVS_READONLY,
            &h) == ESP_OK) {

        (void)nvs_get_str(
            h,
            "master",
            saved,
            &n
        );

        nvs_close(h);
    }

    return strcmp(pin, saved) == 0;
}

/* ============================================================
 * LOCAL EVENT LOG
 * ============================================================ */

static void log_event(
    const char *method,
    const char *id)
{
    nvs_handle_t h;

    if (nvs_open(
            "logs",
            NVS_READWRITE,
            &h) == ESP_OK) {

        char value[64];

        snprintf(
            value,
            sizeof(value),
            "%s:%s:%" PRId64,
            method,
            id,
            esp_timer_get_time() / 1000000
        );

        (void)nvs_set_str(
            h,
            "latest",
            value
        );

        (void)nvs_commit(h);
        nvs_close(h);
    }
}

/* ============================================================
 * ATTENDANCE STATE
 * ============================================================ */

static bool attendance_ready(void)
{
    LOCK();

    bool ready =
        !admin_authenticated &&
        !admin_pin_entry_active &&
        current_enroll_state == ENROLL_NONE &&
        !result_active;

    UNLOCK();

    return ready;
}

/* Unified attendance function for RFID, FINGER and FACE. */

static bool grant(
    const char *method,
    const char *id)
{
    int64_t now = esp_timer_get_time();

    LOCK();

    bool ready =
        !admin_authenticated &&
        !admin_pin_entry_active &&
        current_enroll_state == ENROLL_NONE &&
        !result_active;

    if (!ready ||
        now - last_accept_us < ATTENDANCE_RESULT_US) {

        UNLOCK();
        return false;
    }

    last_accept_us = now;

    result_active = true;
    result_until_us =
        now + ATTENDANCE_RESULT_US;

    if (
        method &&
        strcmp(method, "RFID") == 0
    ) {
        rfid_need_release = true;
    }

    if (
        method &&
        strcmp(method, "FINGER") == 0
    ) {
        fp_need_release = true;
    }

    UNLOCK();

    ESP_LOGI(
        TAG,
        "=== AUTHENTICATION SUCCESSFUL! %s, ID: %s ===",
        method,
        id
    );

    tft_result_screen(
        method,
        id,
        true
    );

    google_sheet_log_attendance(
        id,
        method,
        "ESP32-GATE-1"
    );

    buzzer_notify_success();

    return true;
}

/* ============================================================
 * RC522 SPI FUNCTIONS
 * ============================================================ */

static uint8_t rcr(uint8_t reg)
{
    if (spi_bus_mutex)
        xSemaphoreTake(
            spi_bus_mutex,
            portMAX_DELAY
        );

    uint8_t tx[2] = {
        (uint8_t)((reg << 1) | 0x80),
        0
    };

    uint8_t rx[2] = {0};

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx
    };

    esp_err_t err =
        spi_device_transmit(rc522, &t);

    if (spi_bus_mutex)
        xSemaphoreGive(spi_bus_mutex);

    if (err != ESP_OK)
        return 0;

    return rx[1];
}

static void wcr(
    uint8_t reg,
    uint8_t value)
{
    if (spi_bus_mutex)
        xSemaphoreTake(
            spi_bus_mutex,
            portMAX_DELAY
        );

    uint8_t tx[2] = {
        (uint8_t)(reg << 1),
        value
    };

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx
    };

    esp_err_t err =
        spi_device_transmit(rc522, &t);

    if (spi_bus_mutex)
        xSemaphoreGive(spi_bus_mutex);

    ESP_ERROR_CHECK(err);
}

static void rset(
    uint8_t reg,
    uint8_t mask)
{
    wcr(reg, rcr(reg) | mask);
}

static void rclr(
    uint8_t reg,
    uint8_t mask)
{
    wcr(reg, rcr(reg) & ~mask);
}

/* ============================================================
 * RC522 INITIALIZATION
 * ============================================================ */

static void rc522_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .max_transfer_sz = TFT_W * TFT_H * 2,
    };

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = PIN_RC522_CS,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            RC522_HOST,
            &bus,
            SPI_DMA_CH_AUTO
        )
    );

    ESP_ERROR_CHECK(
        spi_bus_add_device(
            RC522_HOST,
            &dev,
            &rc522
        )
    );

    wcr(0x01, 0x0F);
    vTaskDelay(pdMS_TO_TICKS(50));

    wcr(0x2A, 0x8D);
    wcr(0x2B, 0x3E);
    wcr(0x2D, 30);
    wcr(0x2C, 0);
    wcr(0x15, 0x40);

    rset(0x14, 0x03);
}

/* ============================================================
 * RC522 TRANSACTION
 * ============================================================ */

static int rc522_xfer_bits(
    const uint8_t *send,
    int slen,
    uint8_t *back,
    int *blen,
    uint8_t tx_last_bits)
{
    wcr(0x01, 0x00);
    wcr(0x02, 0x77);
    wcr(0x04, 0x7F);
    rclr(0x0A, 0x80);

    for (int i = 0; i < slen; ++i) {
        wcr(0x09, send[i]);
    }

    wcr(
        0x0D,
        tx_last_bits & 0x07
    );

    wcr(0x01, 0x0C);
    rset(0x0D, 0x80);

    const int64_t deadline =
        esp_timer_get_time() + 25000;

    uint8_t irq = 0;

    do {
        irq = rcr(0x04);

        if (irq & 0x01)
            break;

        vTaskDelay(
            pdMS_TO_TICKS(1)
        );

    } while (
        !(irq & 0x30) &&
        esp_timer_get_time() < deadline
    );

    rclr(0x0D, 0x80);

    if (
        !(irq & 0x30) ||
        (rcr(0x06) & 0x1B)
    ) {
        return -1;
    }

    int count = rcr(0x0A);

    if (count > *blen)
        return -1;

    for (int i = 0; i < count; ++i) {
        back[i] = rcr(0x09);
    }

    *blen = count;

    return count ? 0 : -1;
}

static int rc522_xfer(
    const uint8_t *send,
    int slen,
    uint8_t *back,
    int *blen)
{
    return rc522_xfer_bits(
        send,
        slen,
        back,
        blen,
        0
    );
}

static bool rc522_poll(
    uint8_t uid[RFID_UID_LEN])
{
    uint8_t answer[18];

    int n = sizeof(answer);

    const uint8_t request[] = {0x26};

    if (
        rc522_xfer_bits(
            request,
            sizeof(request),
            answer,
            &n,
            7
        ) != 0 ||
        n != 2
    ) {
        return false;
    }

    const uint8_t anticollision[] = {
        0x93,
        0x20
    };

    n = sizeof(answer);

    if (
        rc522_xfer(
            anticollision,
            sizeof(anticollision),
            answer,
            &n
        ) != 0 ||
        n < 5
    ) {
        return false;
    }

    if (
        (uint8_t)(
            answer[0] ^
            answer[1] ^
            answer[2] ^
            answer[3]
        ) != answer[4]
    ) {
        return false;
    }

    memcpy(
        uid,
        answer,
        RFID_UID_LEN
    );

    return true;
}

/* ============================== AS608 ============================== */
/*
 * Returns ESP_OK when the UART packet is valid. The AS608's own
 * success/failure code lives in *status; every I/O error must NOT be
 * turned into "no finger present".
 */
static esp_err_t as608_exec(uint8_t cmd, const uint8_t *data, size_t len,
                             uint8_t *reply, size_t *rlen, uint8_t *status)
{
    if (len > 20 || (!data && len) || !reply || !rlen || !status)
        return ESP_ERR_INVALID_ARG;
    if (as608_mutex) xSemaphoreTake(as608_mutex, portMAX_DELAY);
    uint8_t packet[32] = {0xef, 0x01, 0xff, 0xff, 0xff, 0xff, 0x01,
                          (uint8_t)((len + 3) >> 8), (uint8_t)(len + 3), cmd};
    uint16_t sum = 1 + packet[7] + packet[8] + cmd;
    for (size_t i = 0; i < len; ++i) { packet[10 + i] = data[i]; sum += data[i]; }
    packet[10 + len] = (uint8_t)(sum >> 8);
    packet[11 + len] = (uint8_t)sum;

    uart_flush_input(AS608_UART);
    if (uart_write_bytes(AS608_UART, (const char *)packet, 12 + len) !=
        (int)(12 + len)) {
        if (as608_mutex) xSemaphoreGive(as608_mutex);
        return ESP_FAIL;
    }
    uint8_t header[9];
    if (uart_read_bytes(AS608_UART, header, sizeof(header), pdMS_TO_TICKS(800)) != sizeof(header) ||
        header[0] != 0xef || header[1] != 0x01 || header[6] != 0x07) {
        if (as608_mutex) xSemaphoreGive(as608_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int packet_len = ((header[7] << 8) | header[8]);
    int body_len = packet_len - 2; /* confirmation + parameters */
    /*
     * IMPORTANT FIX: the uart_read_bytes call below reads "body_len + 2"
     * bytes (the extra 2 are the checksum) into `reply`. The previous
     * check only compared body_len > *rlen, so when body_len == *rlen
     * (e.g. == 32, exactly sizeof(reply[32])) the read would write 34
     * bytes into an array that only holds 32 -> stack overflow. A noisy
     * UART frame (loose wire, electrical noise) can absolutely produce a
     * body_len value that close to the boundary before the checksum
     * below gets checked.
     */
    if (packet_len < 3 || body_len < 1 || body_len + 2 > (int)*rlen) {
        if (as608_mutex) xSemaphoreGive(as608_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    if (uart_read_bytes(AS608_UART, reply, body_len + 2,
                        pdMS_TO_TICKS(500)) != body_len + 2) {
        if (as608_mutex) xSemaphoreGive(as608_mutex);
        return ESP_ERR_TIMEOUT;
    }

    uint16_t checksum = 0;
    for (int i = 6; i < 9; ++i) checksum += header[i];
    for (int i = 0; i < body_len; ++i) checksum += reply[i];
    if (checksum != (uint16_t)((reply[body_len] << 8) | reply[body_len + 1])) {
        if (as608_mutex) xSemaphoreGive(as608_mutex);
        return ESP_ERR_INVALID_CRC;
    }
    *rlen = body_len;
    *status = reply[0];
    if (as608_mutex) xSemaphoreGive(as608_mutex);
    return ESP_OK;
}

static bool as608_cmd(uint8_t cmd, const uint8_t *data, size_t len,
                      uint8_t *reply, size_t *rlen)
{
    uint8_t status;
    return as608_exec(cmd, data, len, reply, rlen, &status) == ESP_OK && status == AS608_OK;
}

static bool as608_is_online(void)
{
    uint8_t reply[32];
    size_t n = sizeof(reply);
    return as608_cmd(0x0f, NULL, 0, reply, &n); /* ReadSysPara */
}

static void as608_read_capacity(void)
{
    uint8_t reply[32], status;
    size_t n = sizeof(reply);
    if (as608_exec(0x0f, NULL, 0, reply, &n, &status) == ESP_OK &&
        status == AS608_OK && n >= 5) {
        /* ReadSysPara payload: confirmation(0), system ID(1-2),
         * capacity(3-4), security level(5-6). */
        uint16_t capacity = ((uint16_t)reply[3] << 8) | reply[4];
        if (capacity > 0) fp_library_size = capacity;
    }
}

/* Search must scan exactly the sensor's own reported library size. */
static bool as608_search(uint16_t *matched_id, uint16_t *score, uint8_t *status)
{
    uint8_t reply[32];
    const uint8_t request[] = {1, 0, 0,
                               (uint8_t)(fp_library_size >> 8),
                               (uint8_t)fp_library_size};
    size_t n = sizeof(reply);
    *matched_id = 0;
    *score = 0;
    *status = 0xff;
    esp_err_t err = as608_exec(0x04, request, sizeof(request), reply, &n, status);
    if (err != ESP_OK || *status != AS608_OK || n < 5) return false;
    *matched_id = ((uint16_t)reply[1] << 8) | reply[2];
    *score = ((uint16_t)reply[3] << 8) | reply[4];
    return true;
}

static void as608_delete_template(uint16_t id)
{
    uint8_t reply[16];
    uint8_t request[] = {(uint8_t)(id >> 8), (uint8_t)id, 0, 1};
    size_t n = sizeof(reply);
    if (!as608_cmd(0x0c, request, sizeof(request), reply, &n))
        ESP_LOGE(TAG, "Failed to delete template, ID %u", id);
}

/*
 * Fully deletes one employee - fingerprint (AS608) + RFID (NVS), so the old
 * RFID card becomes a completely "blank" card again (no longer mapped to
 * any ID). Only called from keypad_task, after the admin has already
 * authenticated - does not touch current_enroll_state/admin_authenticated,
 * so it does not need state_mutex; the helper functions
 * (as608_delete_template/erase_rfid_from_nvs) are already called unlocked
 * elsewhere in this same pattern.
 * Returns true if a matching RFID record for emp_id was found and removed.
 */
static bool delete_employee(uint16_t emp_id)
{
    uint8_t uid[RFID_UID_LEN];
    bool had_rfid = find_uid_by_emp_id(emp_id, uid);

    as608_delete_template(emp_id); /* safe even if this ID never had a fingerprint */

    if (had_rfid) {
        (void)erase_rfid_from_nvs(uid);
    }

    char id_str[8];
    snprintf(id_str, sizeof(id_str), "%u", emp_id);
    log_event("DEL", id_str);
    return had_rfid;
}

/* ============================== keypad ============================== */
static int64_t last_press_us;
static char last_key;
#define DEBOUNCE_US 150000

static char keypad_scan(void)
{
    static const char keymap[4][4] = {
        {'1','2','3','A'}, {'4','5','6','B'}, {'7','8','9','C'}, {'*','0','#','D'}
    };
    char pressed = 0;
    for (int c = 0; c < 4 && !pressed; ++c) {
        for (int k = 0; k < 4; ++k) gpio_set_level(cols[k], k == c ? 0 : 1);
        esp_rom_delay_us(20);
        for (int r = 0; r < 4; ++r)
            if (gpio_get_level(rows[r]) == 0) { pressed = keymap[c][r]; break; }
    }
    for (int k = 0; k < 4; ++k) gpio_set_level(cols[k], 1);
    if (!pressed) { last_key = 0; return 0; }
    int64_t now = esp_timer_get_time();
    if (pressed == last_key && now - last_press_us < DEBOUNCE_US) return 0;
    last_key = pressed; last_press_us = now;
    return pressed;
}

/* Must be called while state_mutex is already held. */
static void cancel_admin_or_enrollment_locked(void)
{
    /* If Store already ran but verification has not happened yet, do not
     * leave half-finished data behind. The AS608/NVS calls below do not
     * need the mutex held, but we snapshot the values we need before
     * leaving the locked region for the slow I/O calls; here
     * as608_delete_template/erase_rfid_from_nvs do not touch shared state
     * so calling them directly is safe. */
    bool need_rollback = enroll_rfid_pending &&
        (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE ||
         current_enroll_state == ENROLL_WAIT_VERIFY);
    uint16_t rollback_id = enroll_emp_id;
    uint8_t rollback_uid[RFID_UID_LEN];
    memcpy(rollback_uid, enroll_rfid_uid, RFID_UID_LEN);

    admin_authenticated = false;
    admin_pin_entry_active = false;
    current_enroll_state = ENROLL_NONE;
    enroll_rfid_pending = false;
    enroll_emp_id = 0;
    input_len = 0;

    if (need_rollback) {
        UNLOCK();
        as608_delete_template(rollback_id);
        (void)erase_rfid_from_nvs(rollback_uid);
        LOCK();
    }
}

static void cancel_enrollment_keep_admin(void)
{
    LOCK();
    bool need_rollback = enroll_rfid_pending &&
        (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE ||
         current_enroll_state == ENROLL_WAIT_VERIFY);
    uint16_t rollback_id = enroll_emp_id;
    uint8_t rollback_uid[RFID_UID_LEN];
    memcpy(rollback_uid, enroll_rfid_uid, RFID_UID_LEN);

    current_enroll_state = ENROLL_NONE;
    enroll_rfid_pending = false;
    enroll_emp_id = 0;
    input_len = 0;
    UNLOCK();

    if (need_rollback) {
        as608_delete_template(rollback_id);
        (void)erase_rfid_from_nvs(rollback_uid);
    }
}

static void cancel_admin_or_enrollment(void)
{
    LOCK();
    cancel_admin_or_enrollment_locked();
    UNLOCK();
    tft_ready_screen();
}

/* ---- Admin screens (display only - does not touch the original logic) ---- */
static void ui_show_pin_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("ADMIN LOGIN", "PIN CODE:", shown,
                     "B:DEL  C:CONFIRM", "#:BACK");
}

static void ui_show_menu(unsigned selection)
{
    const char *actions[] = { "ADD EMPLOYEE", "DELETE EMPLOYEE", "FACE VIA WEB" };
    tft_input_screen("ADMIN MENU", "SELECT ACTION", actions[selection % 3],
                     "A:NEXT  C:SELECT", "B:BACK  #:EXIT");
}

static void ui_show_add_id_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("ADD EMPLOYEE", "ENTER ID:", shown,
                     "B:DEL  C:CONFIRM", "#:BACK");
}

static void ui_show_del_id_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("DELETE EMPLOYEE", "ENTER ID:", shown,
                     "B:DEL  C:CONFIRM", "#:BACK");
}

static void ui_show_face_id_entry(void)
{
    char shown[12];
    size_t n = input_len < sizeof(shown) - 1 ? input_len : sizeof(shown) - 1;
    memcpy(shown, input_buffer, n); shown[n] = 0;
    tft_input_screen("ENROLL FACE", "EXISTING EMP ID", shown,
                     "B:DEL  C:START", "#:CANCEL");
}

static void keypad_task(void *arg)
{
    bool entering_master = false;
    admin_ui_mode_t admin_ui_mode = ADMIN_UI_NONE;
    unsigned menu_selection = 0;
    int64_t last_ui_action_us = esp_timer_get_time();

    while (true) {
        char key = keypad_scan();

        LOCK();
        bool is_admin = admin_authenticated;
        enroll_state_t enroll_now = current_enroll_state;
        UNLOCK();

        /* Enrollment has its own longer watchdog. Do not let the shorter
         * admin-menu idle timer cancel a sensor operation while the user is
         * following the fingerprint prompts. */
        if (is_admin && enroll_now == ENROLL_NONE &&
            esp_timer_get_time() - last_ui_action_us >= ADMIN_IDLE_TIMEOUT_US) {
            ESP_LOGI(TAG, "Admin idle for 30s -> back to attendance mode");
            cancel_admin_or_enrollment();
            entering_master = false;
            admin_ui_mode = ADMIN_UI_NONE;
            menu_selection = 0;
            last_ui_action_us = esp_timer_get_time();
            continue;
        }

        if (!key) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_ui_action_us = esp_timer_get_time();

        LOCK();
        is_admin = admin_authenticated;
        enroll_now = current_enroll_state;
        UNLOCK();

        /* # = immediately cancel whatever is in progress and go back to
         * the attendance screen. */
        if (key == '#') {
            if (enroll_now == ENROLL_WAIT_FACE) {
                uint16_t pending; LOCK(); pending = enroll_emp_id; UNLOCK();
                cam_send_command("ENROLL_CANCEL", pending);
            }
            cancel_admin_or_enrollment();
            entering_master = false;
            admin_ui_mode = ADMIN_UI_NONE;
            menu_selection = 0;
            last_ui_action_us = esp_timer_get_time();
            continue;
        }

        /* * = enter admin mode. */
        if (key == '*') {
            if (enroll_now == ENROLL_WAIT_FACE) {
                uint16_t pending; LOCK(); pending = enroll_emp_id; UNLOCK();
                cam_send_command("ENROLL_CANCEL", pending);
            }
            cancel_admin_or_enrollment();
            LOCK();
            admin_pin_entry_active = true;
            UNLOCK();
            entering_master = true;
            admin_ui_mode = ADMIN_UI_NONE;
            menu_selection = 0;
            input_len = 0;
            ui_show_pin_entry();
            continue;
        }

        if (entering_master) {
            if (key == 'B') {
                if (input_len > 0) {
                    input_len--;
                    ui_show_pin_entry();
                }
            } else if (key == 'C') {
                input_buffer[input_len] = 0;
                if (nvs_pin_ok(input_buffer)) {
                    LOCK();
                    admin_authenticated = true;
                    admin_pin_entry_active = false;
                    UNLOCK();
                    entering_master = false;
                    input_len = 0;
                    admin_ui_mode = ADMIN_UI_MENU;
                    menu_selection = 0;
                    ui_show_menu(0);
                } else {
                    input_len = 0;
                    tft_message_color("FAILED", "WRONG PIN",
                                      "RETRY", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(700));
                    ui_show_pin_entry();
                }
            } else if (key >= '0' && key <= '9' &&
                       input_len < sizeof(input_buffer) - 1) {
                input_buffer[input_len++] = key;
                ui_show_pin_entry();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!is_admin) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Currently enrolling: B goes back to the admin menu, only # exits
         * all the way back to attendance mode. */
        if (enroll_now != ENROLL_NONE) {
            if (key == 'B') {
                if (enroll_now == ENROLL_WAIT_FACE) {
                    uint16_t pending; LOCK(); pending = enroll_emp_id; UNLOCK();
                    cam_send_command("ENROLL_CANCEL", pending);
                }
                cancel_enrollment_keep_admin();
                admin_ui_mode = ADMIN_UI_MENU;
                menu_selection = 0;
                ui_show_menu(0);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (admin_ui_mode == ADMIN_UI_MENU) {
            if (key == 'A') {
                menu_selection = (menu_selection + 1) % 3;
                ui_show_menu(menu_selection);
            } else if (key == 'B') {
                cancel_admin_or_enrollment();
                admin_ui_mode = ADMIN_UI_NONE;
                menu_selection = 0;
                tft_ready_screen();
            } else if (key == 'C') {
                input_len = 0;
                if (menu_selection == 1) {
                    admin_ui_mode = ADMIN_UI_ENTER_DEL_ID;
                    ui_show_del_id_entry();
                } else if (menu_selection == 2) {
                    /* Website enrollment is the only path for face enrollment. */
                    admin_ui_mode = ADMIN_UI_MENU;
                    tft_message("FACE REGISTER", "USE LAPTOP WEB", "ADMIN KEY");
                } else {
                    admin_ui_mode = ADMIN_UI_ENTER_ADD_ID;
                    ui_show_add_id_entry();
                }
            }
        } else if (admin_ui_mode == ADMIN_UI_ENTER_ADD_ID) {
            if (key == 'B') {
                if (input_len > 0) {
                    input_len--;
                    ui_show_add_id_entry();
                } else {
                    admin_ui_mode = ADMIN_UI_MENU;
                    ui_show_menu(0);
                }
            } else if (key == 'C') {
                input_buffer[input_len] = 0;
                long id = strtol(input_buffer, NULL, 10);

                uint8_t enrolled_uid[RFID_UID_LEN];
                bool id_already_enrolled = id > 0 && id < fp_library_size &&
                    find_uid_by_emp_id((uint16_t)id, enrolled_uid);
                if (id > 0 && id < fp_library_size && !id_already_enrolled) {
                    LOCK();
                    enroll_emp_id = (uint16_t)id;
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_WAIT_RFID;
                    enroll_started_us = esp_timer_get_time();
                    UNLOCK();

                    input_len = 0;
                    admin_ui_mode = ADMIN_UI_MENU;
                    tft_message("ADD EMPLOYEE", "SCAN RFID CARD",
                                "B:BACK");
                } else {
                    if (id_already_enrolled)
                        ESP_LOGW(TAG, "Employee ID %ld already has an RFID mapping", id);
                    tft_message_color("FAILED", "INVALID ID",
                                       "RE-ENTER", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(700));
                    ui_show_add_id_entry();
                }
            } else if (key >= '0' && key <= '9' &&
                       input_len < sizeof(input_buffer) - 1) {
                input_buffer[input_len++] = key;
                ui_show_add_id_entry();
            }
        } else if (admin_ui_mode == ADMIN_UI_ENTER_DEL_ID) {
            if (key == 'B') {
                if (input_len > 0) {
                    input_len--;
                    ui_show_del_id_entry();
                } else {
                    admin_ui_mode = ADMIN_UI_MENU;
                    ui_show_menu(1);
                }
            } else if (key == 'C') {
                input_buffer[input_len] = 0;
                long id = strtol(input_buffer, NULL, 10);

                if (id > 0 && id < fp_library_size) {
                    /* Fail closed BEFORE removing RFID/AS608; keep a durable
                     * remote-delete request until laptop ACKs it. */
                    esp_err_t flag_err = face_nvs_set("face", (uint16_t)id, false);
                    if (flag_err != ESP_OK) {
                        tft_message_color("FAILED", "FACE NVS ERROR", "RETRY", RESULT_RED);
                        continue;
                    }
                    esp_err_t pending_err = face_nvs_set("face_del", (uint16_t)id, true);
                    if (pending_err != ESP_OK) {
                        tft_message_color("FAILED", "DELETE QUEUE ERR", "RETRY", RESULT_RED);
                        continue;
                    }
                    LOCK();
                    bitmap_del(face_roster,(uint16_t)id);
                    bitmap_del(face_staging,(uint16_t)id);
                    UNLOCK();
                    bool existed = delete_employee((uint16_t)id);
                    cam_send_command("FACE_DELETE", (uint16_t)id);
                    char id_str[12];
                    snprintf(id_str, sizeof(id_str), "ID %ld", id);

                    if (existed)
                        tft_message("DELETE OK", id_str, "RFID & FP REMOVED");
                    else
                        tft_message("DELETE OK", id_str, "NO RFID FOUND");

                    vTaskDelay(pdMS_TO_TICKS(1200));
                    admin_ui_mode = ADMIN_UI_MENU;
                    menu_selection = 0;
                    ui_show_menu(0);
                } else {
                    tft_message_color("FAILED", "INVALID ID",
                                       "RE-ENTER", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(700));
                    ui_show_del_id_entry();
                }
            } else if (key >= '0' && key <= '9' &&
                       input_len < sizeof(input_buffer) - 1) {
                input_buffer[input_len++] = key;
                ui_show_del_id_entry();
            }
        } else if (admin_ui_mode == ADMIN_UI_ENTER_FACE_ID) {
            if (key == 'B') {
                if (input_len > 0) { input_len--; ui_show_face_id_entry(); }
                else { admin_ui_mode = ADMIN_UI_MENU; menu_selection = 2; ui_show_menu(2); }
            } else if (key == 'C') {
                input_buffer[input_len] = 0;
                long id = strtol(input_buffer, NULL, 10);
                uint8_t uid[RFID_UID_LEN];
                bool valid = id > 0 && id < fp_library_size &&
                             find_uid_by_emp_id((uint16_t)id, uid);
                if (!valid) {
                    tft_message_color("FAILED", "ID NOT REGISTERED", "ADD RFID+FP FIRST", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    ui_show_face_id_entry();
                } else if (face_nvs_get("face_del", (uint16_t)id)) {
                    tft_message_color("WAIT", "SYNC FACE DELETE", "CHECK LAPTOP", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    ui_show_face_id_entry();
                } else if (face_nvs_set("face", (uint16_t)id, false) != ESP_OK) {
                    tft_message_color("FAILED", "NVS WRITE ERROR", "RETRY", RESULT_RED);
                } else {
                    LOCK();
                    current_enroll_state = ENROLL_WAIT_FACE;
                    enroll_emp_id = (uint16_t)id;
                    enroll_started_us = esp_timer_get_time();
                    UNLOCK();
                    admin_ui_mode = ADMIN_UI_MENU;
                    input_len = 0;
                    cam_send_command("ENROLL_START", (uint16_t)id);
                    tft_message("ENROLL FACE", "WAIT CAMERA/SERVER", "TURN HEAD: 30 PICS");
                }
            } else if (key >= '0' && key <= '9' && input_len < 5) {
                input_buffer[input_len++] = key;
                ui_show_face_id_entry();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Background task: if the admin presses B and then walks away without
 * finishing (never presses D), the system automatically exits the enroll
 * state after ENROLL_TIMEOUT_US instead of locking attendance forever. */
static void enroll_watchdog_task(void *arg)
{
    while (true) {
        LOCK();
        bool face_pending = (current_enroll_state == ENROLL_WAIT_FACE);
        uint16_t pending_id = enroll_emp_id;
        int64_t limit = face_pending ? FACE_ENROLL_TIMEOUT_US : ENROLL_TIMEOUT_US;
        bool expired = current_enroll_state != ENROLL_NONE &&
            (esp_timer_get_time() - enroll_started_us) > limit;
        UNLOCK();
        if (expired) {
            if (face_pending) cam_send_command("ENROLL_CANCEL", pending_id);
            ESP_LOGW(TAG, "Enrollment timed out, auto-cancelled");
            cancel_admin_or_enrollment();
            tft_message_color("FAILED", "ENROLLMENT CANCELLED", "READY", RESULT_RED);
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (attendance_ready()) tft_ready_screen();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void ui_housekeeping_task(void *arg)
{
    int64_t last_clock_refresh = 0;

    while (true) {
        int64_t now = esp_timer_get_time();

        LOCK();
        bool active = result_active;
        int64_t until = result_until_us;
        UNLOCK();

        if (active && now >= until) {
            LOCK();
            result_active = false;
            UNLOCK();
            if (attendance_ready()) tft_ready_screen();
        }

        if (!active && attendance_ready() &&
            now - last_clock_refresh >= 1000000LL) {
            last_clock_refresh = now;
            tft_ready_screen();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================== Tasks ============================== */
static void rfid_task(void *arg)
{
    uint8_t uid[RFID_UID_LEN];
    char id[12];
    while (true) {
        bool card_present = rc522_poll(uid);
        if (!card_present) {
            LOCK();
            if (rfid_need_release) rfid_need_release = false;
            UNLOCK();
        } else {
            LOCK();
            bool in_enroll_wait_rfid = (current_enroll_state == ENROLL_WAIT_RFID) &&
                                        admin_authenticated;
            uint16_t enroll_id_snapshot = enroll_emp_id;
            UNLOCK();

            if (in_enroll_wait_rfid) {
                uint16_t existing = check_rfid_in_nvs(uid);
                if (existing != 0) {
                    ESP_LOGW(TAG, "RFID card already belongs to ID %u", existing);
                    tft_message("CARD EXISTS", "SCAN OTHER CARD", "B:CANCEL");
                } else if (!as608_is_online()) {
                    /* Do not save the card if the fingerprint sensor is offline. */
                    ESP_LOGE(TAG, "AS608 not responding; RFID not saved");
                    tft_message("FINGERPRINT ERROR", "CHECK AS608", "SCAN CARD AGAIN");
                } else {
                    LOCK();
                    /* Re-check the state is still correct after the slow I/O
                     * above (avoid overwriting if the admin already pressed
                     * B or the watchdog already cancelled while we waited). */
                    if (current_enroll_state == ENROLL_WAIT_RFID &&
                        enroll_emp_id == enroll_id_snapshot) {
                        memcpy(enroll_rfid_uid, uid, sizeof(uid));
                        enroll_rfid_pending = true;
                        current_enroll_state = ENROLL_WAIT_FINGER_1;
                        enroll_started_us = esp_timer_get_time();
                        UNLOCK();
                        ESP_LOGI(TAG, "RFID OK; AS608 OK. Place finger 1st time");
                        tft_message("CARD OK", "PLACE FINGER", "STEP 1");
                    } else {
                        UNLOCK();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(800));
            } else if (attendance_ready()) {
                LOCK();
                bool blocked = rfid_need_release;
                UNLOCK();
                if (blocked) {
                    /* Only unlock RFID once the card has been pulled away. */
                    continue;
                }
                uint16_t emp = check_rfid_in_nvs(uid);
                if (emp) {
                    snprintf(id, sizeof(id), "%u", emp);
                    (void)grant("RFID", id);
                } else {
                    ESP_LOGW(TAG, "Unregistered RFID card");
                    tft_result_screen("RFID", "", false);
                }
                vTaskDelay(pdMS_TO_TICKS(800));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void fingerprint_task(void *arg)
{
    uint8_t reply[32];
    while (true) {
        size_t n = sizeof(reply);

        LOCK();
        enroll_state_t st = current_enroll_state;
        uint16_t emp_id_snapshot = enroll_emp_id;
        UNLOCK();

        if (st == ENROLL_WAIT_FINGER_1) {
            uint8_t status = 0xff;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_OK) {
                uint8_t buffer = 1; n = sizeof(reply); status = 0xff;
                err = as608_exec(0x02, &buffer, 1, reply, &n, &status);
                if (err == ESP_OK && status == AS608_OK) {
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_1) {
                        current_enroll_state = ENROLL_WAIT_FINGER_REMOVE;
                        enroll_started_us = esp_timer_get_time();
                    }
                    UNLOCK();
                    ESP_LOGI(TAG, "Fingerprint 1 OK; lift finger");
                    tft_message("FINGER OK", "LIFT FINGER", "WAIT FOR STEP 2");
                } else {
                    ESP_LOGW(TAG, "AS608 image2Tz step 1 failed: err=%s status=0x%02X",
                             esp_err_to_name(err), status);
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_1)
                        current_enroll_state = ENROLL_WAIT_RETRY_REMOVE;
                    UNLOCK();
                    tft_message("PLACE AGAIN", "SAME FINGER", "STEP 1");
                }
            } else if (err == ESP_OK && status != AS608_NO_FINGER) {
                ESP_LOGW(TAG, "AS608 image capture step 1 failed: status=0x%02X", status);
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_FINGER_1)
                    current_enroll_state = ENROLL_WAIT_RETRY_REMOVE;
                UNLOCK();
                tft_message("PLACE AGAIN", "SAME FINGER", "STEP 1");
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 UART error at enrollment step 1: %s",
                         esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_FINGER_REMOVE) {
            /* Only status 0x02 really means the finger has been lifted. */
            uint8_t status;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_NO_FINGER) {
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_FINGER_REMOVE) {
                    current_enroll_state = ENROLL_WAIT_FINGER_2;
                    enroll_started_us = esp_timer_get_time();
                }
                UNLOCK();
                tft_message("PLACE AGAIN", "SAME FINGER", "STEP 2");
                vTaskDelay(pdMS_TO_TICKS(400));
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 UART error while waiting for lift: %s", esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_FINGER_2) {
            uint8_t status = 0xff;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_OK) {
                uint8_t buffer = 2; n = sizeof(reply); status = 0xff;
                err = as608_exec(0x02, &buffer, 1, reply, &n, &status);
                if (err != ESP_OK || status != AS608_OK) {
                    ESP_LOGW(TAG, "AS608 image2Tz step 2 failed: err=%s status=0x%02X",
                             esp_err_to_name(err), status);
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_2)
                        current_enroll_state = ENROLL_WAIT_RETRY_REMOVE;
                    UNLOCK();
                    tft_message("PLACE AGAIN", "SAME FINGER", "STEP 1");
                    goto next;
                }
                n = sizeof(reply); status = 0xff;
                err = as608_exec(0x05, NULL, 0, reply, &n, &status);
                if (err != ESP_OK || status != AS608_OK) {
                    ESP_LOGW(TAG, "AS608 RegModel failed: err=%s status=0x%02X",
                             esp_err_to_name(err), status);
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_2) {
                        current_enroll_state = ENROLL_WAIT_RETRY_REMOVE;
                        enroll_started_us = esp_timer_get_time();
                    }
                    UNLOCK();
                    if (err == ESP_OK)
                        tft_message("MISMATCH", "RESTART STEP 1", "RETRY");
                    goto next;
                }
                uint8_t store[] = {1, (uint8_t)(emp_id_snapshot >> 8), (uint8_t)emp_id_snapshot};
                n = sizeof(reply);
                status = 0xff;
                err = as608_exec(0x06, store, sizeof(store), reply, &n, &status);
                if (err != ESP_OK || status != AS608_OK) {
                    ESP_LOGE(TAG, "Failed to store AS608 template");
                    ESP_LOGE(TAG, "AS608 Store failed: err=%s status=0x%02X",
                             esp_err_to_name(err), status);
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_FINGER_2)
                        current_enroll_state = ENROLL_WAIT_RETRY_REMOVE;
                    UNLOCK();
                    tft_message("SAVE ERROR", "RETRY", "B:CANCEL");
                    goto next;
                }

                bool save_ok;
                LOCK();
                save_ok = enroll_rfid_pending &&
                          current_enroll_state == ENROLL_WAIT_FINGER_2;
                uint8_t uid_to_save[RFID_UID_LEN];
                memcpy(uid_to_save, enroll_rfid_uid, RFID_UID_LEN);
                UNLOCK();

                if (save_ok && save_rfid_to_nvs(uid_to_save, emp_id_snapshot) == ESP_OK) {
                    LOCK();
                    save_ok = enroll_rfid_pending &&
                              current_enroll_state == ENROLL_WAIT_FINGER_2 &&
                              enroll_emp_id == emp_id_snapshot;
                    if (save_ok) {
                        /* Store only confirms the data was written to flash, not
                         * that Search can actually find it. Require lifting the
                         * finger and scanning a 3rd time to verify for real. */
                        current_enroll_state = ENROLL_WAIT_VERIFY_REMOVE;
                        enroll_started_us = esp_timer_get_time();
                    }
                    UNLOCK();
                } else {
                    save_ok = false;
                }
                if (!save_ok) {
                    ESP_LOGE(TAG, "Template saved but RFID not written/committed to NVS");
                    as608_delete_template(emp_id_snapshot);
                    (void)erase_rfid_from_nvs(uid_to_save);
                    LOCK();
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_NONE;
                    admin_authenticated = false;
                    UNLOCK();
                }

                if (save_ok) {
                    tft_message("TEMPLATE SAVED", "LIFT FINGER", "VERIFYING NEXT");
                } else {
                    tft_message("NVS ERROR", "CARD NOT SAVED", "CHECK NVS");
                }
            } else if (err == ESP_OK && status != AS608_NO_FINGER) {
                ESP_LOGW(TAG, "AS608 image capture step 2 failed: status=0x%02X", status);
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_FINGER_2)
                    current_enroll_state = ENROLL_WAIT_RETRY_REMOVE;
                UNLOCK();
                tft_message("PLACE AGAIN", "SAME FINGER", "STEP 1");
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 UART error at enrollment step 2: %s",
                         esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_RETRY_REMOVE) {
            uint8_t status = 0xff;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_NO_FINGER) {
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_RETRY_REMOVE) {
                    current_enroll_state = ENROLL_WAIT_FINGER_1;
                    enroll_started_us = esp_timer_get_time();
                }
                UNLOCK();
                tft_message("PLACE AGAIN", "SAME FINGER", "STEP 1");
                vTaskDelay(pdMS_TO_TICKS(400));
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 UART error while waiting to retry enrollment: %s",
                         esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_VERIFY_REMOVE) {
            uint8_t status;
            esp_err_t err = as608_exec(0x01, NULL, 0, reply, &n, &status);
            if (err == ESP_OK && status == AS608_NO_FINGER) {
                LOCK();
                if (current_enroll_state == ENROLL_WAIT_VERIFY_REMOVE) {
                    current_enroll_state = ENROLL_WAIT_VERIFY;
                    enroll_started_us = esp_timer_get_time();
                }
                UNLOCK();
                tft_message("VERIFY", "PLACE FINGER AGAIN", "STEP 3");
                vTaskDelay(pdMS_TO_TICKS(400));
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "AS608 UART error while waiting for verify: %s", esp_err_to_name(err));
            }
        } else if (st == ENROLL_WAIT_VERIFY) {
            if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (!as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    ESP_LOGW(TAG, "Failed to extract features for verification");
                    goto next;
                }
                uint16_t matched, score;
                uint8_t status;
                bool verify_ok = as608_search(&matched, &score, &status) &&
                                  matched == emp_id_snapshot;

                if (verify_ok) {
                    LOCK();
                    if (current_enroll_state == ENROLL_WAIT_VERIFY) {
                        enroll_rfid_pending = false;
                        current_enroll_state = ENROLL_NONE;
                        admin_authenticated = false;
                    }
                    UNLOCK();
                    ESP_LOGI(TAG, "VERIFICATION COMPLETE: ID %u, score %u", matched, score);
                    tft_message("ADD EMPLOYEE", "VERIFIED OK", "READY");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    tft_ready_screen();
                } else {
                    /* Never leave behind an account that can enroll but
                     * cannot actually authenticate. */
                    ESP_LOGE(TAG, "Verification failed: status=0x%02X, match=%u, expected=%u",
                             status, matched, emp_id_snapshot);
                    uint8_t uid_snapshot[RFID_UID_LEN];
                    LOCK();
                    memcpy(uid_snapshot, enroll_rfid_uid, RFID_UID_LEN);
                    enroll_rfid_pending = false;
                    current_enroll_state = ENROLL_NONE;
                    admin_authenticated = false;
                    UNLOCK();
                    as608_delete_template(emp_id_snapshot);
                    (void)erase_rfid_from_nvs(uid_snapshot);
                    tft_message_color("FAILED", "DATA ROLLED BACK", "RETRY", RESULT_RED);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    if (attendance_ready()) tft_ready_screen();
                }
            }
        } else if (attendance_ready()) {
            LOCK();
            bool blocked = fp_need_release;
            UNLOCK();

            if (blocked) {
                uint8_t release_status = 0xff;
                size_t rn = sizeof(reply);
                esp_err_t re = as608_exec(0x01, NULL, 0, reply, &rn,
                                          &release_status);
                if (re == ESP_OK && release_status == AS608_NO_FINGER) {
                    LOCK();
                    fp_need_release = false;
                    UNLOCK();
                }
            } else if (as608_cmd(0x01, NULL, 0, reply, &n)) {
                uint8_t buffer = 1; n = sizeof(reply);
                if (as608_cmd(0x02, &buffer, 1, reply, &n)) {
                    uint16_t matched, score;
                    uint8_t status;
                    if (as608_search(&matched, &score, &status)) {
                        char id[8]; snprintf(id, sizeof(id), "%u", matched);
                        ESP_LOGI(TAG, "Fingerprint match ID=%u score=%u", matched, score);
                        (void)grant("FINGER", id);
                    } else {
                        ESP_LOGW(TAG, "Fingerprint no match (AS608 status=0x%02X)", status);
                        tft_result_screen("FINGER", "", false);
                        vTaskDelay(pdMS_TO_TICKS(700));
                        if (attendance_ready()) tft_ready_screen();
                    }
                }
            }
        }
next:
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* Binary UART1 frames are parsed by exactly one cam_task.
 * Other tasks may send COMMAND frames but never call uart_read_bytes(CAM_UART).
 */
static bool face_id_parse(const char *s, uint16_t *id)
{
    if (!s || !*s || strlen(s) > 5) return false;
    unsigned long val = 0;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        val = val * 10 + (unsigned)(*p - '0');
    }
    if (val == 0 || val >= fp_library_size) return false;
    *id = (uint16_t)val;
    return true;
}

static bool face_enroll_state_is(uint16_t id)
{
    LOCK();
    bool active = admin_authenticated && current_enroll_state == ENROLL_WAIT_FACE &&
                  enroll_emp_id == id;
    UNLOCK();
    return active;
}

static void face_enroll_finish(uint16_t id, bool ok)
{
    uint8_t uid[RFID_UID_LEN];
    /* Check RFID existence first; final commit is protected by state_mutex
     * so cancelling enrollment cannot race a late ENROLL_DONE packet. */
    if (ok) ok = find_uid_by_emp_id(id, uid);
    LOCK();
    if (!(admin_authenticated && current_enroll_state == ENROLL_WAIT_FACE &&
          enroll_emp_id == id)) {
        UNLOCK();
        return;
    }
    if (ok) ok = face_nvs_set("face", id, true) == ESP_OK;
    current_enroll_state = ENROLL_NONE;
    admin_authenticated = false;
    enroll_emp_id = 0;
    UNLOCK();

    char label[18];
    snprintf(label, sizeof(label), "EMP ID %u", (unsigned)id);

    if (ok)
        tft_message("FACE ENROLLED", label, "3 METHODS READY");
    else
        tft_message_color("FAILED", label, "RETRY ENROLL FACE", RESULT_RED);

    vTaskDelay(pdMS_TO_TICKS(1500));
    if (attendance_ready()) tft_ready_screen();
}

/* Website pending request arrives as a transport STATUS event from S3.
 * UART replies remain distinct from app-level employee authorization. */
static bool check_fp_template_exists(uint16_t id)
{
    uint8_t req[3] = {
        2,
        (uint8_t)(id >> 8),
        (uint8_t)id
    };

    uint8_t reply[16];
    uint8_t status = 0xff;
    size_t n = sizeof(reply);

    return as608_exec(
        0x07,
        req,
        sizeof(req),
        reply,
        &n,
        &status
    ) == ESP_OK && status == AS608_OK;
}

/* ============================================================
 * WEBSITE EMPLOYEE CHECK + FACE ROSTER SYNC
 * ============================================================ */

static bool face_handle_web_status(const char *s)
{
    unsigned id = 0;
    unsigned gen = 0;
    unsigned cnt = 0;
    int end = 0;
    char token[9] = {0};

    /* Website requests validation of an employee ID. */
    if (strncmp(s, "CHECK_EMP:", 10) == 0) {
        if (
            sscanf(
                s,
                "CHECK_EMP:%u:%8[0-9a-f]%n",
                &id,
                token,
                &end
            ) != 2 ||
            s[end] != '\0' ||
            strlen(token) != 8 ||
            id == 0 ||
            id >= fp_library_size
        ) {
            ESP_LOGW(TAG, "Bad CHECK_EMP request");
            return true;
        }

        uint8_t uid[RFID_UID_LEN];

        LOCK();

        bool idle =
            !admin_authenticated &&
            !admin_pin_entry_active &&
            current_enroll_state == ENROLL_NONE;

        UNLOCK();

        bool exists =
            idle &&
            !face_nvs_get("face_del", (uint16_t)id) &&
            find_uid_by_emp_id((uint16_t)id, uid) &&
            check_fp_template_exists((uint16_t)id);

        char cmd[48];

        snprintf(
            cmd,
            sizeof(cmd),
            "EMP_%s:%u:%s",
            exists ? "OK" : "NO",
            id,
            token
        );

        if (!cam_send_line(cmd)) {
            ESP_LOGE(
                TAG,
                "CHECK_EMP response queue full"
            );
        }

        ESP_LOGI(
            TAG,
            "CHECK_EMP ID=%u result=%s",
            id,
            exists ? "OK" : "NO"
        );

        return true;
    }

    /* A new roster is constructed in the staging bitmap. */
    if (strncmp(s, "FACE_SYNC_BEGIN:", 16) == 0) {
        if (
            sscanf(
                s,
                "FACE_SYNC_BEGIN:%u:%u%n",
                &gen,
                &cnt,
                &end
            ) != 2 ||
            s[end] != '\0' ||
            gen == 0 ||
            cnt > 512
        ) {
            ESP_LOGW(TAG, "Bad roster BEGIN");
            return true;
        }

        LOCK();

        /* Preserve active roster until the new one is complete. */
        memset(
            face_staging,
            0,
            sizeof(face_staging)
        );

        face_receiving = true;
        face_roster_gen = gen;
        face_roster_expected = cnt;
        face_roster_seen = 0;
        face_roster_last_id = 0;

        UNLOCK();

        return true;
    }

    /* Receive one ID from the server's face roster. */
    if (strncmp(s, "FACE_SYNC_ID:", 13) == 0) {
        if (
            sscanf(
                s,
                "FACE_SYNC_ID:%u%n",
                &id,
                &end
            ) != 1 ||
            s[end] != '\0' ||
            id == 0 ||
            id > 65535
        ) {
            return true;
        }

        LOCK();

        if (
            !face_receiving ||
            face_roster_seen >= face_roster_expected ||
            id <= face_roster_last_id
        ) {
            face_receiving = false;
        } else {
            bitmap_put(
                face_staging,
                (uint16_t)id
            );

            face_roster_last_id = id;
            face_roster_seen++;
        }

        UNLOCK();

        return true;
    }

    /* Commit the new roster only when fully received. */
    if (strncmp(s, "FACE_SYNC_END:", 14) == 0) {
        if (
            sscanf(
                s,
                "FACE_SYNC_END:%u:%u%n",
                &gen,
                &cnt,
                &end
            ) != 2 ||
            s[end] != '\0'
        ) {
            return true;
        }

        LOCK();

        bool valid =
            face_receiving &&
            gen == face_roster_gen &&
            cnt == face_roster_expected &&
            face_roster_seen == cnt;

        if (valid) {
            memcpy(
                face_roster,
                face_staging,
                sizeof(face_roster)
            );

            face_roster_at_us = esp_timer_get_time();
            face_roster_ready = true;
        }

        face_receiving = false;

        UNLOCK();

        if (valid) {
            ESP_LOGI(
                TAG,
                "FACE ROSTER SYNC PASS count=%u",
                cnt
            );
        } else {
            ESP_LOGW(
                TAG,
                "FACE ROSTER REJECTED"
            );
        }

        return true;
    }

    return false;
}

/* ============================================================
 * CAMERA STATUS HANDLER
 * ============================================================ */

static void cam_handle_status(const char *line)
{
    /* New website employee verification and roster sync. */
    if (face_handle_web_status(line)) {
        return;
    }

    uint16_t id = 0;

    if (!strncmp(line, "ENROLL_PROGRESS:", 16)) {

        char idstr[8] = {0};
        unsigned count = 0;

        if (
            sscanf(
                line,
                "ENROLL_PROGRESS:%7[0-9]:%u",
                idstr,
                &count
            ) == 2 &&
            face_id_parse(idstr, &id) &&
            face_enroll_state_is(id)
        ) {
            char msg[24];

            snprintf(
                msg,
                sizeof(msg),
                "CAPTURE %u/30",
                count
            );

            tft_message(
                "ENROLL FACE",
                msg,
                "TURN HEAD SLOWLY"
            );
        }

    } else if (!strncmp(line, "ENROLL_STARTED:", 15)) {

        if (
            face_id_parse(line + 15, &id) &&
            face_enroll_state_is(id)
        ) {
            tft_message(
                "ENROLL FACE",
                "CAMERA CONNECTED",
                "30 IMAGES / 1 SEC"
            );
        }

    } else if (!strncmp(line, "ENROLL_DONE:", 12)) {

        if (face_id_parse(line + 12, &id)) {
            face_enroll_finish(id, true);
        }

    } else if (!strncmp(line, "ENROLL_ERROR:", 13)) {

        if (face_id_parse(line + 13, &id)) {
            face_enroll_finish(id, false);
        }

    } else if (!strncmp(line, "FACE_DELETED:", 13)) {

        if (face_id_parse(line + 13, &id)) {

            if (
                face_nvs_set(
                    "face_del",
                    id,
                    false
                ) != ESP_OK
            ) {
                ESP_LOGW(
                    TAG,
                    "Could not clear remote-delete tombstone ID=%u",
                    (unsigned)id
                );
            }
        }

    } else if (!strcmp(line, "FACE_UNKNOWN")) {

        /* Unknown face:
         * Red feedback only.
         * Never log as successful attendance.
         */

        LOCK();

        bool can_show =
            !admin_authenticated &&
            !admin_pin_entry_active &&
            current_enroll_state == ENROLL_NONE &&
            !result_active;

        if (can_show) {
            result_active = true;

            result_until_us =
                esp_timer_get_time() +
                ATTENDANCE_RESULT_US;
        }

        UNLOCK();

        if (can_show) {
            tft_result_screen(
                "FACE",
                "",
                false
            );
        }

    } else if (!strcmp(line, "CAM_READY")) {

        ESP_LOGI(
            TAG,
            "S3 camera UART ready"
        );

        face_retry_pending_delete();
    }
}

/* ============================================================
 * FACE AUTHENTICATION
 *
 * Payload:
 *
 * Byte 0-1    : Employee ID (little-endian)
 * Byte 2-18433: RGB565 96x96 face image
 *
 * Total: 18434 bytes.
 * ============================================================ */

static void cam_handle_face(
    const uint8_t *payload,
    size_t len)
{
    if (len != FL_MAX_PAYLOAD) {
        ESP_LOGW(
            TAG,
            "Invalid FACE payload length=%u",
            (unsigned)len
        );
        return;
    }

    uint16_t emp =
        (uint16_t)payload[0] |
        ((uint16_t)payload[1] << 8);

    if (
        emp == 0 ||
        !face_employee_authorized(emp)
    ) {
        ESP_LOGW(
            TAG,
            "FACE event denied id=%u (not enrolled/authorized)",
            emp
        );

        return;
    }

    memcpy(
        face_photo,
        payload + 2,
        FACE_PHOTO_LEN
    );

    char idstr[11];

    snprintf(
        idstr,
        sizeof(idstr),
        "%u",
        (unsigned)emp
    );

    /* Same attendance function as RFID/fingerprint. */

    if (grant("FACE", idstr)) {
        tft_face_photo_screen(idstr);
    }
}

/* ============================================================
 * CAMERA UART RECEIVER
 *
 * UART1:
 *   MAIN RX GPIO32 <- S3 TX GPIO43
 *   MAIN TX GPIO33 -> S3 RX GPIO44
 *
 * Frame:
 *   UTST + type + seq + length + CRC32 + payload
 *
 * Handles:
 *   HELLO / READY
 *   FACE
 *   STATUS
 *   ACK / NACK
 *
 * ACK means frame accepted at transport level.
 * It does NOT mean attendance is authorized.
 * ============================================================ */

static void cam_task(void *arg)
{
    (void)arg;

    fl_header_t h;

    uint32_t expected_seq = 1;
    uint32_t last_seq = 0;
    uint32_t last_crc = 0;
    uint32_t last_type = 0;

    uint32_t pass_count = 0;
    uint32_t fail_count = 0;
    uint32_t duplicate_count = 0;

    ESP_LOGI(
        TAG,
        "CAM UART1 framed RX started (GPIO32), buffer=%u",
        (unsigned)FL_MAX_PAYLOAD
    );

    while (true) {

        fl_rx_result_t r = fl_read_frame(
            CAM_UART,
            &h,
            face_rx_payload,
            sizeof(face_rx_payload),
            1000,
            6500
        );

        if (r == FL_RX_NONE) {
            continue;
        }

        /* Handle corrupted/incomplete frames. */

        if (r != FL_RX_OK) {

            ++fail_count;

            ESP_LOGW(
                TAG,
                "CAM RX error code=%d type=%" PRIu32
                " seq=%" PRIu32
                " len=%" PRIu32,
                (int)r,
                h.type,
                h.seq,
                h.len
            );

            if (
                h.seq &&
                (
                    h.type == FL_TYPE_FACE ||
                    h.type == FL_TYPE_STATUS
                )
            ) {
                (void)fl_send_frame(
                    CAM_UART,
                    cam_tx_mutex,
                    FL_TYPE_NACK,
                    h.seq,
                    NULL,
                    0,
                    0
                );
            }

            if (
                r == FL_RX_BAD_HEADER ||
                r == FL_RX_INCOMPLETE
            ) {
                uart_flush_input(CAM_UART);
            }

            continue;
        }

        /* ----------------------------------------------------
         * HELLO -> READY handshake
         * ---------------------------------------------------- */

        if (
            h.type == FL_TYPE_HELLO &&
            h.seq == 0 &&
            h.len == 0
        ) {
            LOCK();

            face_roster_ready = false;
            face_receiving = false;

            UNLOCK();

            cam_link_ready = false;

            expected_seq = 1;

            last_seq = 0;
            last_crc = 0;
            last_type = 0;

            pass_count = 0;
            fail_count = 0;
            duplicate_count = 0;

            if (
                fl_send_frame(
                    CAM_UART,
                    cam_tx_mutex,
                    FL_TYPE_READY,
                    0,
                    NULL,
                    0,
                    0
                )
            ) {
                cam_link_ready = true;

                ESP_LOGI(
                    TAG,
                    "CAM UART HANDSHAKE PASS"
                );

            } else {

                ESP_LOGE(
                    TAG,
                    "CAM UART READY send failed"
                );
            }

            continue;
        }

        /* ----------------------------------------------------
         * Command ACK/NACK from S3
         * ---------------------------------------------------- */

        if (
            h.type == FL_TYPE_ACK ||
            h.type == FL_TYPE_NACK
        ) {
            cam_reply_t reply = {
                h.type,
                h.seq,
                h.crc
            };

            (void)xQueueSend(
                cam_reply_q,
                &reply,
                0
            );

            continue;
        }

        if (
            h.type != FL_TYPE_FACE &&
            h.type != FL_TYPE_STATUS
        ) {
            continue;
        }

        /* ----------------------------------------------------
         * Sequence and duplicate checking
         * ---------------------------------------------------- */

        bool duplicate =
            h.seq == last_seq &&
            h.crc == last_crc &&
            h.type == last_type;

        if (
            !duplicate &&
            h.seq != expected_seq
        ) {
            ++fail_count;

            ESP_LOGW(
                TAG,
                "CAM UART sequence mismatch expected=%" PRIu32
                " actual=%" PRIu32,
                expected_seq,
                h.seq
            );

            (void)fl_send_frame(
                CAM_UART,
                cam_tx_mutex,
                FL_TYPE_NACK,
                h.seq,
                NULL,
                0,
                0
            );

            continue;
        }

        /* ----------------------------------------------------
         * Payload validation
         * ---------------------------------------------------- */

        if (
            (
                h.type == FL_TYPE_FACE &&
                h.len != FL_MAX_PAYLOAD
            ) ||
            (
                h.type == FL_TYPE_STATUS &&
                (
                    h.len == 0 ||
                    h.len > 110
                )
            )
        ) {
            ++fail_count;

            (void)fl_send_frame(
                CAM_UART,
                cam_tx_mutex,
                FL_TYPE_NACK,
                h.seq,
                NULL,
                0,
                0
            );

            continue;
        }

        if (duplicate) {

            ++duplicate_count;

        } else {

            last_seq = h.seq;
            last_crc = h.crc;
            last_type = h.type;

            expected_seq++;
            pass_count++;
        }

        /* ----------------------------------------------------
         * Transport-level ACK
         * ---------------------------------------------------- */

        bool ack_ok = fl_send_frame(
            CAM_UART,
            cam_tx_mutex,
            FL_TYPE_ACK,
            h.seq,
            NULL,
            0,
            h.crc
        );

        if (!ack_ok) {

            ESP_LOGW(
                TAG,
                "CAM UART ACK TX failed seq=%" PRIu32,
                h.seq
            );
        }

        /* Already-processed packet: ACK again, do not
         * execute business logic twice.
         */

        if (duplicate) {
            continue;
        }

        /* ----------------------------------------------------
         * Dispatch FACE / STATUS
         * ---------------------------------------------------- */

        if (h.type == FL_TYPE_FACE) {

            ESP_LOGI(
                TAG,
                "FACE RX CRC PASS seq=%" PRIu32
                " bytes=%" PRIu32,
                h.seq,
                h.len
            );

            cam_handle_face(
                face_rx_payload,
                h.len
            );

        } else {

            char status[112];

            memcpy(
                status,
                face_rx_payload,
                h.len
            );

            status[h.len] = '\0';

            ESP_LOGI(
                TAG,
                "CAM STATUS: %s",
                status
            );

            cam_handle_status(status);
        }

        if (
            pass_count > 0 &&
            pass_count % 20 == 0
        ) {
            ESP_LOGI(
                TAG,
                "CAM LINK stats pass=%" PRIu32
                " fail=%" PRIu32
                " duplicate=%" PRIu32,
                pass_count,
                fail_count,
                duplicate_count
            );
        }
    }
}

/* ============================================================
 * LD2410C PRESENCE SENSOR
 *
 * Detects whether a person is in front of the gate.
 *
 * PRESENCE:1 -> Person detected
 * PRESENCE:0 -> No person detected
 *
 * Detection state is forwarded to S3-CAM via UART.
 * ============================================================ */

static void presence_task(void *arg)
{
    (void)arg;

    int old = -1;

    int64_t last_retry = 0;

    while (true) {

        int now = gpio_get_level(
            PIN_LD2410_OUT
        );

        if (now != old) {

            old = now;

            (void)cam_send_line(
                now ?
                "PRESENCE:1" :
                "PRESENCE:0"
            );

            ESP_LOGI(
                TAG,
                "LD2410C presence=%d",
                now
            );
        }

        int64_t now_us =
            esp_timer_get_time();

        /* Re-send pending remote face deletions. */

        if (
            now_us - last_retry >
            5000000LL
        ) {
            last_retry = now_us;

            face_retry_pending_delete();
        }

        vTaskDelay(
            pdMS_TO_TICKS(100)
        );
    }
}

/* ============================================================
 * MAIN PROGRAM
 * ============================================================ */

void app_main(void)
{
    /* --------------------------------------------------------
     * Create synchronization primitives
     * -------------------------------------------------------- */

    state_mutex = xSemaphoreCreateMutex();

    tft_mutex = xSemaphoreCreateMutex();

    spi_bus_mutex = xSemaphoreCreateMutex();

    as608_mutex = xSemaphoreCreateMutex();

    cam_tx_mutex = xSemaphoreCreateMutex();

    cam_command_q = xQueueCreate(
        16,
        sizeof(cam_command_t)
    );

    cam_reply_q = xQueueCreate(
        16,
        sizeof(cam_reply_t)
    );

    if (
        !state_mutex ||
        !tft_mutex ||
        !spi_bus_mutex ||
        !as608_mutex ||
        !cam_tx_mutex ||
        !cam_command_q ||
        !cam_reply_q
    ) {
        ESP_LOGE(
            TAG,
            "Failed to create system mutexes - halting"
        );

        abort();
    }

    buzzer_init();

    /* --------------------------------------------------------
     * Initialize NVS
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
     * Wi-Fi and NTP
     * -------------------------------------------------------- */

    wifi_time_init();

    /* --------------------------------------------------------
     * Google Sheets logging
     *
     * Must initialize before attendance tasks.
     * -------------------------------------------------------- */

    if (!google_sheet_init()) {

        ESP_LOGE(
            TAG,
            "Google Sheet logging disabled (init failed)"
        );
    }

    /* --------------------------------------------------------
     * LD2410C
     * -------------------------------------------------------- */

    gpio_config_t presence = {
        .pin_bit_mask =
            1ULL << PIN_LD2410_OUT,

        .mode = GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(
        gpio_config(&presence)
    );

    /* --------------------------------------------------------
     * 4x4 keypad
     * -------------------------------------------------------- */

    for (int i = 0; i < 4; ++i) {

        ESP_ERROR_CHECK(
            gpio_set_direction(
                cols[i],
                GPIO_MODE_OUTPUT
            )
        );

        ESP_ERROR_CHECK(
            gpio_set_level(
                cols[i],
                1
            )
        );

        ESP_ERROR_CHECK(
            gpio_set_direction(
                rows[i],
                GPIO_MODE_INPUT
            )
        );

        ESP_ERROR_CHECK(
            gpio_set_pull_mode(
                rows[i],
                GPIO_PULLUP_ONLY
            )
        );
    }

    /* --------------------------------------------------------
     * AS608 UART2
     * -------------------------------------------------------- */

    uart_config_t uart_cfg = {
        .baud_rate = AS608_BAUD,

        .data_bits =
            UART_DATA_8_BITS,

        .parity =
            UART_PARITY_DISABLE,

        .stop_bits =
            UART_STOP_BITS_1,

        .flow_ctrl =
            UART_HW_FLOWCTRL_DISABLE,

        .source_clk =
            UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(
        uart_param_config(
            AS608_UART,
            &uart_cfg
        )
    );

    ESP_ERROR_CHECK(
        uart_set_pin(
            AS608_UART,
            PIN_AS608_TX,
            PIN_AS608_RX,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    ESP_ERROR_CHECK(
        uart_driver_install(
            AS608_UART,
            512,
            0,
            0,
            NULL,
            0
        )
    );

    /* --------------------------------------------------------
     * ESP32-S3-CAM UART1
     * -------------------------------------------------------- */

    uart_cfg.baud_rate = CAM_BAUD;

    ESP_ERROR_CHECK(
        uart_param_config(
            CAM_UART,
            &uart_cfg
        )
    );

    ESP_ERROR_CHECK(
        uart_set_pin(
            CAM_UART,
            PIN_CAM_TX,
            PIN_CAM_RX,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    ESP_ERROR_CHECK(
        uart_driver_install(
            CAM_UART,
            32768,
            4096,
            0,
            NULL,
            0
        )
    );

    /* --------------------------------------------------------
     * SPI peripherals
     * -------------------------------------------------------- */

    rc522_init();
    tft_init();

    /* --------------------------------------------------------
     * AS608 status
     * -------------------------------------------------------- */

    if (as608_is_online()) {

        as608_read_capacity();

        ESP_LOGI(
            TAG,
            "AS608 online, template capacity: %u",
            fp_library_size
        );

    } else {

        ESP_LOGE(
            TAG,
            "AS608 not responding - check TX/RX, GND and AS608_BAUD"
        );
    }

    /* --------------------------------------------------------
     * Create FreeRTOS tasks
     * -------------------------------------------------------- */

    xTaskCreate(
        rfid_task,
        "rfid",
        4096,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        fingerprint_task,
        "finger",
        4096,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        keypad_task,
        "keypad",
        3072,
        NULL,
        4,
        NULL
    );

    xTaskCreate(
        cam_task,
        "cam",
        4096,
        NULL,
        5,
        NULL
    );

    xTaskCreate(
        cam_command_task,
        "cam_tx",
        4096,
        NULL,
        4,
        NULL
    );

    xTaskCreate(
        presence_task,
        "presence",
        2048,
        NULL,
        4,
        NULL
    );

    xTaskCreate(
        enroll_watchdog_task,
        "enroll_wd",
        2048,
        NULL,
        3,
        NULL
    );

    xTaskCreate(
        ui_housekeeping_task,
        "ui_house",
        3072,
        NULL,
        3,
        NULL
    );

    ESP_LOGI(
        TAG,
        "============================================"
    );

    ESP_LOGI(
        TAG,
        "SYSTEM READY."
    );

    ESP_LOGI(
        TAG,
        "ADMIN: * | A:next | B:back/delete | C:select | #:exit"
    );

    ESP_LOGI(
        TAG,
        "============================================"
    );
}
