#ifndef APSTA_SHRIMP_CONFIG_STORE_H
#define APSTA_SHRIMP_CONFIG_STORE_H

#include <stdint.h>
#include "esp_err.h"

struct theConf {
    char sta_ssid[60];
    char sta_pass[12];
    bool conntype;
    float WTemp;
    float IRLevel;
    float SalinityLevel;
    float PHLevel;
    float DOLevel;
    uint32_t retrycount;
    uint32_t lifecount;
    uint8_t poolid;
    uint8_t unitid;
    uint8_t interval;
    uint8_t retry;
    bool DOSensor;
    bool PHSensor;
    bool SalinitySensor;
    bool IRsensor;
    uint32_t sentinel;
};

extern struct theConf theConf;
esp_err_t save_theconf_to_nvs(void);

#endif // APSTA_SHRIMP_CONFIG_STORE_H
