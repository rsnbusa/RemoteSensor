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

// #define BATMAXLEVEL (12.0f)
// UART / RS485 transport
// see schematics for wiring details in docs/Schematics GPIOs
#define UART485_PORT UART_NUM_1
#define UART485_BAUD_RATE 9600
#define UART485_TX_PIN GPIO_NUM_42
#define UART485_RX_PIN GPIO_NUM_2
// #define UART485_TX_PIN GPIO_NUM_18
// #define UART485_RX_PIN GPIO_NUM_17
#ifdef SLEEP
#define UART485_RTS_PIN UART_PIN_NO_CHANGE
#else
#define UART485_RTS_PIN GPIO_NUM_48
#endif
#define UART485_CTS_PIN UART_PIN_NO_CHANGE
#define UART485_RX_BUF_SIZE 256

// voltage divider Resistors for 8.4  bat expected level
#define RESISTOR1 (22000.0f)
#define RESISTOR2 (3300.0f)
// DO sampling and retry policy
#define MINDO (4.5f)
#define WAITDO (10000)
#define MAX485_DE GPIO_NUM_14
#define MAX485_RE GPIO_NUM_13           // old pcb
// #define MAX485_DE GPIO_NUM_8
// #define MAX485_RE GPIO_NUM_19
#define NTW GPIO_NUM_41
#define MAXRETRY_MAX485 (3)
// GPIOs for External Power Source for 3 sensors ALL HAVE PULL DOWN 1K RESISTORS AT the Transistor Base
#define SENSOR1 GPIO_NUM_48 // for old pcb
// #define SENSOR1 GPIO_NUM_15
#define SENSOR2 GPIO_NUM_16
#define SENSOR3 GPIO_NUM_7
#define DOPOWER SENSOR1         //for compilation reasons, we will use SENSOR1 pin to power the DO sensor, but in the actual hardware we will have a separate transistor to control power to the DO sensor, so this pin will be used to control that transistor

// ADC configuration
#define BATSOC GPIO_NUM_1
#define ADC_GPIO BATSOC
#define ADC_VREF 1.1f  // Reference voltage in volts
#define BATTERY_CAL_FACTOR 1.034f
#define LOW_BATTERY_THRESHOLD (6.8f)
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
