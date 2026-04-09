/**
 * @file webserver.cpp
 * @brief Web server implementation for IoT device configuration and monitoring
 * 
 * This module provides a web interface using Mongoose for configuring and monitoring
 * the IoT device. It handles:
 * - Device initial configuration and authentication
 * - System settings (WiFi, MQTT, security)
 * - Profile management (schedules and cycles)
 * - Modbus device configuration (inverter, sensors, battery, panels)
 * - System status and statistics
 * - Device reboot control
 * 
 * @author rsn
 * @date Dec 29, 2019
 */
#ifndef TYPESweb_H_
#define TYPESweb_H_
#define GLOBAL
#include <string.h>
#include "defines.h"
#include "mongoose/mongoose_glue.h"
#include "config_store.h"

// Configuration state constants
// mongoose glue structures. 
//
// Delete the STATIC in the mongoose_glue.c or linker fails cannot find externals below
// AND AND double to float conversion in mongoose_glue.h
//
// below are the structures for the mongoose interface as well as Actions (independent from data)

extern struct Sensors s_Sensors;

static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
	if (dst == NULL || dst_size == 0) {
		return;
	}

	if (src == NULL) {
		dst[0] = '\0';
		return;
	}

	size_t len = strnlen(src, dst_size - 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/**
 * @brief Get current system settings for web interface
 * @param data Pointer to system structure to populate
 */
void my_get_Sensors(struct Sensors *data) 
{
	if (data == NULL) {
		return;
	}

	data->pool = theConf.poolid;
	data->unit = theConf.unitid;
	data->interval = theConf.interval;
	data->retry = theConf.retry;
	data->DO = theConf.DOSensor;
	data->PH = theConf.PHSensor;
	data->Sal = theConf.SalinitySensor;
	data->IR = theConf.IRsensor;
	data->lifecount=theConf.lifecount;
	data->retrycount=theConf.retrycount;
	data->IRLevel=theConf.IRLevel;
	data->SALevel=theConf.SalinityLevel;
	data->PHLevel=theConf.PHLevel;
	data->DOLevel=theConf.DOLevel;
	data->WTemp=theConf.WTemp;
	data->batVolts=theConf.batVolts;
	data->conntype=theConf.conntype;	// printf("Settings requested: pool=%u unit=%u interval=%u retry=%u DO=%d PH=%d Sal=%d IR=%d\n",
	copy_bounded(data->mqttssid, sizeof(data->mqttssid), theConf.sta_ssid);
	copy_bounded(data->mqttpassw, sizeof(data->mqttpassw), theConf.sta_pass);
	// 	   data->pool, data->unit, data->interval, data->retry,
	// 	   data->DO, data->PH, data->Sal, data->IR);
}


/**
 * @brief Apply system settings from web interface
 * @param data Pointer to system settings structure
 */
void my_set_Sensors(struct Sensors *data) {
	if (data == NULL) {
		return;
	}

	theConf.poolid = data->pool;
	theConf.unitid = data->unit;
	theConf.interval = data->interval;
	theConf.retry = data->retry;
	theConf.DOSensor = data->DO;
	theConf.PHSensor = data->PH;
	theConf.SalinitySensor = data->Sal;
	theConf.IRsensor = data->IR;
	theConf.conntype=data->conntype;
	theConf.batVolts=data->batVolts;
	copy_bounded(theConf.sta_ssid, sizeof(theConf.sta_ssid), data->mqttssid);
	copy_bounded(theConf.sta_pass, sizeof(theConf.sta_pass), data->mqttpassw);
	// printf("Settings updated: pool=%u unit=%u interval=%u retry=%u DO=%d PH=%d Sal=%d IR=%d\n",
	// 	   theConf.poolid, theConf.unitid, theConf.interval, theConf.retry,
	// 	   theConf.DOSensor, theConf.PHSensor, theConf.SalinitySensor, theConf.IRsensor);
	save_theconf_to_nvs(); // Persist configuration to NVS
	vTaskDelay(pdMS_TO_TICKS(1000));
	esp_restart();
}


/**
 * @brief Start and run the web server
 * @param pArg Task parameter (unused)
 * 
 * Initializes the Mongoose web server and registers all HTTP handlers:
 * - settings: Initial device configuration with authentication
 * - system: System settings management (WiFi, MQTT, security)
 * 
 * Runs indefinitely, polling the Mongoose event loop.
 */
void sensor_webserver(void *pArg)
{
	mongoose_init();	
  	mongoose_set_http_handlers("Sensors", my_get_Sensors, my_set_Sensors);		
	for (;;) {
    mongoose_poll();
  	}

}



#endif