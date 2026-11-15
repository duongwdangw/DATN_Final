
#ifndef FACE_UART_LINK_H
#define FACE_UART_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"

#define FL_MAX_IMAGE_BYTES 18432U
#define FL_MAX_PAYLOAD (FL_MAX_IMAGE_BYTES + 2U)
#define FL_HEADER_BYTES 20U

#define FL_TYPE_HELLO 1U
#define FL_TYPE_READY 2U
#define FL_TYPE_FACE 3U
#define FL_TYPE_ACK 4U
#define FL_TYPE_NACK 5U
#define FL_TYPE_COMMAND 6U
#define FL_TYPE_STATUS 7U

typedef struct {
    uint32_t type, seq, len, crc;
} fl_header_t;

typedef enum {
    FL_RX_NONE = 0,
    FL_RX_OK = 1,
    FL_RX_BAD_HEADER = -1,
    FL_RX_INCOMPLETE = -2,
    FL_RX_CRC_ERROR = -3
} fl_rx_result_t;

static inline void fl_le32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t fl_le32_load(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint32_t fl_crc32(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];

        for (unsigned j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^
                  ((crc & 1U) ? 0xEDB88320U : 0U);
        }
    }

    return ~crc;
}

static inline bool fl_send_frame(
    uart_port_t port,
    SemaphoreHandle_t tx_mutex,
    uint32_t type,
    uint32_t seq,
    const void *payload,
    uint32_t len,
    uint32_t crc)
{
    if (len > FL_MAX_PAYLOAD || (len && !payload))
        return false;

    if (tx_mutex &&
        xSemaphoreTake(tx_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
        return false;

    uint8_t h[FL_HEADER_BYTES] = {'U', 'T', 'S', 'T'};

    fl_le32_store(h + 4, type);
    fl_le32_store(h + 8, seq);
    fl_le32_store(h + 12, len);
    fl_le32_store(h + 16, crc);

    bool ok =
        uart_write_bytes(port, h, (int)sizeof(h))
        == (int)sizeof(h);

    if (ok && len)
        ok = uart_write_bytes(port, payload, len) == (int)len;

    if (ok)
        ok = uart_wait_tx_done(
            port, pdMS_TO_TICKS(5000)) == ESP_OK;

    if (tx_mutex)
        xSemaphoreGive(tx_mutex);

    return ok;
}

static inline bool fl_read_n(
    uart_port_t port,
    uint8_t *dst,
    uint32_t len,
    uint32_t timeout_ms)
{
    uint32_t got = 0;

    int64_t until =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (got < len && esp_timer_get_time() < until) {
        uint32_t to_read = len - got;

        if (to_read > 512)
            to_read = 512;

        int n = uart_read_bytes(
            port,
            dst + got,
            to_read,
            pdMS_TO_TICKS(20) + 1
        );

        if (n > 0)
            got += (uint32_t)n;
    }

    return got == len;
}

static inline fl_rx_result_t fl_read_frame(
    uart_port_t port,
    fl_header_t *out,
    uint8_t *payload,
    uint32_t capacity,
    uint32_t header_timeout_ms,
    uint32_t payload_timeout_ms)
{
    const uint8_t magic[4] = {'U', 'T', 'S', 'T'};
    uint8_t window[4] = {0};
    uint8_t byte = 0;
    uint8_t rest[16];

    int64_t until =
        esp_timer_get_time() +
        (int64_t)header_timeout_ms * 1000;

    memset(out, 0, sizeof(*out));

    while (esp_timer_get_time() < until) {
        if (uart_read_bytes(
                port, &byte, 1,
                pdMS_TO_TICKS(20) + 1) != 1)
            continue;

        memmove(window, window + 1, 3);
        window[3] = byte;

        if (memcmp(window, magic, sizeof(magic)) == 0)
            break;
    }

    if (memcmp(window, magic, sizeof(magic)) != 0)
        return FL_RX_NONE;

    if (!fl_read_n(port, rest, sizeof(rest), 1000))
        return FL_RX_INCOMPLETE;

    out->type = fl_le32_load(rest);
    out->seq  = fl_le32_load(rest + 4);
    out->len  = fl_le32_load(rest + 8);
    out->crc  = fl_le32_load(rest + 12);

    if (out->type < FL_TYPE_HELLO ||
        out->type > FL_TYPE_STATUS ||
        out->len > capacity ||
        out->len > FL_MAX_PAYLOAD)
        return FL_RX_BAD_HEADER;

    if (out->len) {
        if (!fl_read_n(
                port, payload, out->len,
                payload_timeout_ms))
            return FL_RX_INCOMPLETE;

        if (fl_crc32(payload, out->len) != out->crc)
            return FL_RX_CRC_ERROR;
    }

    return FL_RX_OK;
}

#endif /* FACE_UART_LINK_H */
