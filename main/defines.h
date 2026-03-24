#ifndef APSTA_SHRIMP_DEFINES_H
#define APSTA_SHRIMP_DEFINES_H

// Feature toggles
#define SLEEP

// Wi-Fi credentials and mode
// #define STA_SSID "shrimp-p-001-u-01"
#define STA_PASS "csttpstt" // password
// #define AP_SSID "DOSensor"
#define AP_PASS "csttpstt"
#define WIFI_USE_APSTA 1

// HTTP client
// Target must be reachable from your upstream network (STA side).
#define HTTP_POST_URL "http://192.168.4.1/api/remoteLevels"
#define HTTP_POST_BODY_MAX_LEN 1024

// Telemetry transport selection
#define MESSAGE_TRANSPORT_HTTP 0
#define MESSAGE_TRANSPORT_MQTT 1

// MQTT transport settings
#define MQTT_STA_SSID "NETLIFE-RSNCasa"
#define MQTT_STA_PASS "csttpstt"
#define MQTT_BROKER_URI "mqtt://64.23.180.233:1883"
#define MQTT_TOPIC "shrimp/DOSensors"
#define MQTT_CONNECT_TIMEOUT_MS 8000

// Deep sleep timings (milliseconds)
// #define DEEP_SLEEP_MS (10000 * 2)
#define DEEP_SLEEP_MS_WIFI (60000 * 1)

// UART / RS485 transport
#define UART485_PORT UART_NUM_1
#define UART485_BAUD_RATE 9600
#define UART485_TX_PIN GPIO_NUM_42
#define UART485_RX_PIN GPIO_NUM_2
#ifdef SLEEP
#define UART485_RTS_PIN UART_PIN_NO_CHANGE
#else
#define UART485_RTS_PIN GPIO_NUM_48
#endif
#define UART485_CTS_PIN UART_PIN_NO_CHANGE
#define UART485_RX_BUF_SIZE 256

// DO sampling and retry policy
#define MINDO (4.5f)
#define WAITDO (10000)
#define MAX485_DE GPIO_NUM_14
#define MAX485_RE GPIO_NUM_13
#define NTW GPIO_NUM_41
#define MAXRETRY_MAX485 (3)

// Modbus frame layout
#define MODBUS_RSP_DATA_OFFSET (3)
#define MODBUS_RSP_DATA_LEN (12)
#define MODBUS_RSP_TOTAL_LEN (MODBUS_RSP_DATA_OFFSET + MODBUS_RSP_DATA_LEN + 2)

// Modbus float byte orders across two registers
#define MODBUS_ORDER_ABCD 0
#define MODBUS_ORDER_BADC 1
#define MODBUS_ORDER_CDAB 2
#define MODBUS_ORDER_DCBA 3

// Device spec: bytes arrive as ABCD, reverse to DCBA before parsing float.
#define MODBUS_FLOAT_ORDER MODBUS_ORDER_DCBA

#endif // APSTA_SHRIMP_DEFINES_H
