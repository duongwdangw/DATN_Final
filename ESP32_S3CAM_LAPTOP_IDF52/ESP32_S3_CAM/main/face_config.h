#pragma once
/* IMPORTANT: fill in values before flash. Both devices/laptop need same Wi-Fi LAN.
 * Keep this file private when sharing project online. */



#define FACE_WIFI_SSID       "Lighthouse"
#define FACE_WIFI_PASSWORD   "261220044"

#define FACE_SERVER_BASE_URL "http://172.20.10.4:5000"
#define FACE_SERVER_URL      FACE_SERVER_BASE_URL "/api/frame"

#define FACE_DEVICE_KEY      "S3CameraDevice_2026_X7p9K2m8R4q6"



/* ESP32-S3-EYE camera pin map as in YOUR tested Arduino example. */
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D0 11
#define CAM_PIN_D1 9
#define CAM_PIN_D2 8
#define CAM_PIN_D3 10
#define CAM_PIN_D4 12
#define CAM_PIN_D5 18
#define CAM_PIN_D6 17
#define CAM_PIN_D7 16
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

/* CHOOSE PINS THAT ARE PHYSICALLY EXPOSED AND UNUSED ON YOUR S3 BOARD.
 * S3 TX -> ESP32 MAIN GPIO32 (RX); S3 RX <- ESP32 MAIN GPIO33 (TX)
 * TX and RX are voltage-level 3.3V, common GND is mandatory. */
#define FACE_S3_TX_GPIO 2
#define FACE_S3_RX_GPIO 1
#define FACE_UART_BAUD 115200

/* Camera frame rate while running; enrollment is restricted to ~1 saved photo/s. */
#define FACE_FRAME_PERIOD_MS 330
#define FACE_CAM_VERTICAL_FLIP 1
#define FACE_CAM_HORIZONTAL_MIRROR 0
#define FACE_THUMB_W 96
#define FACE_THUMB_H 96
#define FACE_THUMB_BYTES (FACE_THUMB_W * FACE_THUMB_H * 2)
