#ifndef INC_H  
#define INC_H

#include <string.h>
#include <stdio.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "defines.h"
#include "config_store.h"
#include <net/if.h>
#include "cJSON.h"

#ifndef DEBUG
#ifdef ESP_LOGE
#undef ESP_LOGE
#endif
#ifdef ESP_LOGW
#undef ESP_LOGW
#endif
#ifdef ESP_LOGI
#undef ESP_LOGI
#endif
#ifdef ESP_LOGD
#undef ESP_LOGD
#endif
#ifdef ESP_LOGV
#undef ESP_LOGV
#endif

#define ESP_LOGE(tag, format, ...) ((void)0)
#define ESP_LOGW(tag, format, ...) ((void)0)
#define ESP_LOGI(tag, format, ...) ((void)0)
#define ESP_LOGD(tag, format, ...) ((void)0)
#define ESP_LOGV(tag, format, ...) ((void)0)
#endif

#endif