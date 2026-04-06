#include "includes.h"

#define MINTIME (60000) // 1 minute in milliseconds

struct theConf theConf= {
    .conntype = MESSAGE_TRANSPORT_HTTP,
    .retrycount=0,
    .lifecount=0,
    .poolid = 1,
    .unitid = 1,
    .interval = 60,
    .retry = 3,
    .DOSensor = true,
    .PHSensor = true,
    .SalinitySensor = true,
    .IRsensor = true
};

static const struct theConf kDefaultConf = {
    .conntype = MESSAGE_TRANSPORT_HTTP,
    .retrycount = 0,
    .lifecount = 0,
    .poolid = 1,
    .unitid = 1,
    .interval = 60,
    .retry = 3,
    .DOSensor = true,
    .PHSensor = true,
    .SalinitySensor = true,
    .IRsensor = true,
    .sentinel = 0xDEADBEEF,
};
char STA_SSID[40],AP_SSID[40];
static const char *TAG = "RemoteSensors";

uint32_t DEEP_SLEEP_MS=60000; // default 1 minute, can be updated by configuration
static bool s_message_sent = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static TaskHandle_t s_ap_blink_task = NULL;
static TaskHandle_t s_web_task = NULL;
static TaskHandle_t s_rs485_task = NULL;
static bool s_ap_client_connected = false;
static RTC_DATA_ATTR float s_do_value_rtc = 0.0f;
static RTC_DATA_ATTR int s_count = 0;
static RTC_DATA_ATTR int s_retry_count = 0;
static RTC_DATA_ATTR bool s_config_mode_latched = false;
static RTC_DATA_ATTR int s_transport_mode_latched = MESSAGE_TRANSPORT_HTTP;

static float avgDO=0.0f,waterTemp=0.0f;
static bool confFlag=false;
static bool s_theconf_invalid = false;
extern void sensor_webserver(void *pArg);
static void ap_assigned_ip_blink_task(void *pvParameters);

static const char *NVS_NS_CFG = "appcfg";
static const char *NVS_KEY_THECONF = "theConf";

#define BOOT_BTN_GPIO GPIO_NUM_0
#define BOOT_BTN_LONG_PRESS_MS 4000
#define BOOT_BTN_POLL_MS 50
#define BOOT_BTN_MAX_MEASURE_MS 12000

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_PUBLISHED_BIT BIT1

static EventGroupHandle_t s_mqtt_event_group = NULL;
static int s_transport_mode = MESSAGE_TRANSPORT_HTTP;

static void copy_cstr_to_u8(uint8_t *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t src_len = strnlen(src, dst_size - 1);
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

static bool start_task_once(TaskFunction_t task_fn,
                            const char *task_name,
                            uint32_t stack_size,
                            UBaseType_t priority,
                            TaskHandle_t *task_handle)
{
    if (task_fn == NULL || task_name == NULL || task_handle == NULL) {
        return false;
    }

    if (*task_handle != NULL) {
        return true;
    }

    BaseType_t ok = xTaskCreate(task_fn, task_name, stack_size, NULL, priority, task_handle);
    if (ok != pdPASS) {
        *task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create %s task", task_name);
        return false;
    }

    return true;
}

static bool woke_from_deep_sleep(esp_sleep_wakeup_cause_t wakeup_reason)
{
    return wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED;
}

static float random_float_range(float min_value, float max_value)
{
    uint32_t r = esp_random();
    float unit = (float)r / 4294967295.0f;
    return min_value + unit * (max_value - min_value);
}

static void blink_mode_indicator(int count)
{
    if (count <= 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        gpio_set_level(NTW, 1);
        vTaskDelay(pdMS_TO_TICKS(140));
        gpio_set_level(NTW, 0);
        vTaskDelay(pdMS_TO_TICKS(160));
    }
}

static esp_err_t force_default_config_mode_and_save(void)
{
    theConf = kDefaultConf;
    s_theconf_invalid = true;
    confFlag = true;
    s_config_mode_latched = true;
    s_transport_mode = MESSAGE_TRANSPORT_HTTP;
    return save_theconf_to_nvs();
}

static int sanitize_transport_mode(int conntype)
{
    if (conntype == MESSAGE_TRANSPORT_HTTP || conntype == MESSAGE_TRANSPORT_MQTT) {
        return conntype;
    }
    return MESSAGE_TRANSPORT_HTTP;
}

static const char *transport_mode_str(int conntype)
{
    switch (sanitize_transport_mode(conntype)) {
    case MESSAGE_TRANSPORT_HTTP:
        return "HTTP";
    case MESSAGE_TRANSPORT_MQTT:
        return "MQTT";
    default:
        return "HTTP";
    }
}
static const char *mqtt_sta_ssid_from_conf(void)
{
    return (strnlen(theConf.sta_ssid, sizeof(theConf.sta_ssid)) > 0) ?
           theConf.sta_ssid : MQTT_STA_SSID;
}

static const char *mqtt_sta_pass_from_conf(void)
{
    return (strnlen(theConf.sta_pass, sizeof(theConf.sta_pass)) > 0) ?
           theConf.sta_pass : MQTT_STA_PASS;
}

static void load_transport_mode_from_conf(void)
{
    s_transport_mode = sanitize_transport_mode(theConf.conntype);
    s_transport_mode_latched = s_transport_mode;
}


static void persist_transport_selection_if_needed(int selected_transport)
{
    int sanitized = sanitize_transport_mode(selected_transport);
    if (theConf.conntype == sanitized) {
        return;
    }

    theConf.conntype = sanitized;
    esp_err_t err = save_theconf_to_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist conntype=%d: %s", sanitized, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Persisted conntype=%d (%s)", sanitized, transport_mode_str(sanitized));
    }
}


static void apply_transport_selection(int transport_mode, bool config_mode)
{
    s_transport_mode = sanitize_transport_mode(transport_mode);
    s_transport_mode_latched = s_transport_mode;
    confFlag = config_mode;
    s_config_mode_latched = config_mode;
    persist_transport_selection_if_needed(s_transport_mode);
}

static void select_boot_mode_and_transport(esp_sleep_wakeup_cause_t wakeup_reason)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // Short delay to allow GPIO state to stabilize after reset
    if (gpio_get_level(BOOT_BTN_GPIO) != 0) {
        load_transport_mode_from_conf();
        confFlag = s_config_mode_latched;
        persist_transport_selection_if_needed(s_transport_mode);
        ESP_LOGW(TAG,
                 "GPIO0 not pressed at boot: transport=%d (%s) from saved conntype (wakeup=%d, config latch=%d)",
                 s_transport_mode,
                 transport_mode_str(s_transport_mode),
                 (int)wakeup_reason,
                 (int)s_config_mode_latched);
        return;
    }

    int held_ms = 0;
    while (gpio_get_level(BOOT_BTN_GPIO) == 0 && held_ms < BOOT_BTN_MAX_MEASURE_MS) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_BTN_POLL_MS));
        held_ms += BOOT_BTN_POLL_MS;
    }

    if (held_ms >= BOOT_BTN_LONG_PRESS_MS) {
        apply_transport_selection(MESSAGE_TRANSPORT_MQTT, false);
        ESP_LOGW(TAG, "GPIO0 long press (%d ms): %s mode", held_ms, transport_mode_str(s_transport_mode));
    } else {
        // Config mode should not overwrite the saved transport choice.
        load_transport_mode_from_conf();
        confFlag = true;
        s_config_mode_latched = true;
        ESP_LOGW(TAG, "GPIO0 short press (%d ms): configuration mode, keeping saved transport=%s", held_ms,
                 transport_mode_str(s_transport_mode));
    }
}

esp_err_t save_theconf_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_CFG, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(save) failed: %s", esp_err_to_name(err));
        return err;
    }
    theConf.sentinel=0xDEADBEEF; // sanity check value to detect uninitialized or corrupted config
    err = nvs_set_blob(nvs, NVS_KEY_THECONF, &theConf, sizeof(theConf));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save theConf failed: %s", esp_err_to_name(err));
        return err;
    }

    // ESP_LOGI(TAG, "Saved theConf to NVS");
    return ESP_OK;
}

static esp_err_t load_theconf_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS_CFG, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(load) failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t required_size = sizeof(theConf);
    err = nvs_get_blob(nvs, NVS_KEY_THECONF, &theConf, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "theConf not found in NVS, writing defaults");
        nvs_close(nvs);
        return force_default_config_mode_and_save();
    }

    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load theConf failed: %s", esp_err_to_name(err));
        return err;
    }

    if (required_size != sizeof(theConf)) {
        ESP_LOGW(TAG, "theConf size mismatch (%u != %u), resetting defaults",
                 (unsigned)required_size, (unsigned)sizeof(theConf));
        return force_default_config_mode_and_save();
    }
    if(theConf.sentinel!=0xDEADBEEF)
    {
        ESP_LOGW(TAG, "theConf sentinel mismatch (0x%08X), resetting defaults", theConf.sentinel);
        return force_default_config_mode_and_save();
    }   
    // ESP_LOGI(TAG,
    //          "Loaded theConf: DO=%d PH=%d SAL=%d IR=%d pool=%u unit=%u interval=%u retry=%u",
    //          theConf.DOSensor, theConf.PHSensor, theConf.SalinitySensor, theConf.IRsensor,
    //          theConf.poolid, theConf.unitid, theConf.interval, theConf.retry);
    return ESP_OK;
}

static float parse_modbus_float(const uint8_t *src)
{
    uint8_t ordered[4] = {0};

#if MODBUS_FLOAT_ORDER == MODBUS_ORDER_ABCD
    ordered[0] = src[0]; ordered[1] = src[1]; ordered[2] = src[2]; ordered[3] = src[3];
#elif MODBUS_FLOAT_ORDER == MODBUS_ORDER_BADC
    ordered[0] = src[1]; ordered[1] = src[0]; ordered[2] = src[3]; ordered[3] = src[2];
#elif MODBUS_FLOAT_ORDER == MODBUS_ORDER_CDAB
    ordered[0] = src[2]; ordered[1] = src[3]; ordered[2] = src[0]; ordered[3] = src[1];
#else // MODBUS_ORDER_DCBA
    ordered[0] = src[3]; ordered[1] = src[2]; ordered[2] = src[1]; ordered[3] = src[0];
#endif

    uint32_t bits = ((uint32_t)ordered[0] << 24) |
                    ((uint32_t)ordered[1] << 16) |
                    ((uint32_t)ordered[2] << 8) |
                    ((uint32_t)ordered[3]);

    float out = 0.0f;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

int DOHandler(const uint8_t *que, uint16_t len)
{
    if (que == NULL || len < MODBUS_RSP_DATA_LEN) {
        ESP_LOGE(TAG, "Invalid DO payload len=%u (expected %u)", len, MODBUS_RSP_DATA_LEN);
        return -1;
    }

    float ftemp = parse_modbus_float(&que[0]);
    float fpercent = parse_modbus_float(&que[4]);
    float fdoval = parse_modbus_float(&que[8]);
    waterTemp=ftemp;
    theConf.DOLevel=fdoval;
    theConf.WTemp=ftemp;
    theConf.IRLevel = random_float_range(0.5f, 8.4f);
    theConf.PHLevel = random_float_range(0.5f, 8.4f);
    theConf.SalinityLevel = random_float_range(0.5f, 8.4f);
    

    s_count++;
    s_do_value_rtc = fdoval; // Store DO value in RTC variable for next wakeup
    ESP_LOGW(TAG,"Temp %0.2fC percent %0.2f%% DO %0.2fmg/L %.02f count %d retries %d",
           ftemp, fpercent * 100.0f, fdoval, s_do_value_rtc, s_count,s_retry_count);
    return 0;
}

static esp_err_t uart485_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART485_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };


    ESP_RETURN_ON_ERROR(uart_driver_install(UART485_PORT, UART485_RX_BUF_SIZE, 0, 0, NULL, 0), TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(UART485_PORT, &uart_config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART485_PORT, UART485_TX_PIN, UART485_RX_PIN, UART485_RTS_PIN, UART485_CTS_PIN), TAG, "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_set_mode(UART485_PORT, UART_MODE_RS485_HALF_DUPLEX), TAG, "uart_set_mode failed");

    // ESP_LOGI(TAG, "UART485 RTU ready on UART%d (8E1, TX=%d RX=%d RTS=%d)", UART485_PORT, UART485_TX_PIN, UART485_RX_PIN, UART485_RTS_PIN);
    return ESP_OK;
}

static esp_err_t app_gpio_outputs_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask =
        (1ULL << MAX485_DE) | (1ULL << MAX485_RE) | (1ULL << NTW) | (1ULL << GPIO_NUM_48);      //DE,RE for RS485 and 48 for Transitor for DOSensor transitor
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config failed");

    // ESP_LOGI(TAG, "Configured MAX485 pins: DE=%d (HIGH), RE=%d (LOW)", MAX485_DE, MAX485_RE);

    rtc_gpio_hold_dis((gpio_num_t)MAX485_RE);       //restore normal pin function on RE pin (allow it to be driven low for receive mode)
    rtc_gpio_hold_dis((gpio_num_t)MAX485_DE);
    gpio_set_level(GPIO_NUM_48, 1);

    vTaskDelay(10); // Short delay to ensure DE/RE levels are stable before UART operations
    return ESP_OK;
}

void set_tx_rs485()
{
    gpio_set_level(MAX485_DE, 1);
    gpio_set_level(MAX485_RE, 1);
    vTaskDelay(pdMS_TO_TICKS(10)); // Short delay to ensure DE/RE levels are stable before UART operations

}
void set_rx_rs485()
{
    gpio_set_level(MAX485_DE, 0);
    gpio_set_level(MAX485_RE, 0);
    vTaskDelay(pdMS_TO_TICKS(10)); // Short delay to ensure DE/RE levels are stable before UART operations

}
void set_sleep_rs485()
{
    gpio_set_level(MAX485_DE, 0);
    gpio_set_level(MAX485_RE, 1);
    gpio_set_level(GPIO_NUM_48, 0);

    ESP_ERROR_CHECK(rtc_gpio_isolate(MAX485_DE));
    ESP_ERROR_CHECK(rtc_gpio_isolate(MAX485_RE));
    gpio_set_level(NTW, 0); // Set NTW high to indicate we're starting a transaction

    vTaskDelay(pdMS_TO_TICKS(10)); // Short delay to ensure DE/RE levels are stable before UART operations

}
static int uart485_send(const uint8_t *data, size_t len)
{
    #ifdef SLEEP
    set_tx_rs485();
    #endif
    return uart_write_bytes(UART485_PORT, data, len);
}

static int uart485_read(uint8_t *data, size_t len, TickType_t timeout_ticks)
{
    #ifdef SLEEP
    set_rx_rs485();
    #endif
    return uart_read_bytes(UART485_PORT, data, len, timeout_ticks);
}

static esp_err_t rs485_send_read_do_request(uint8_t *response, size_t response_size, int *response_len)
{
    static const uint8_t request_frame[] = {0x10, 0x03, 0x20, 0x00, 0x00, 0x06, 0xCD, 0x49};

    if (response == NULL || response_len == NULL || response_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *response_len = 0;

    ESP_RETURN_ON_ERROR(uart_flush(UART485_PORT), TAG, "uart_flush failed");
    gpio_set_level(NTW, 1); // Set NTW high to indicate we're starting a transaction
    int written = uart485_send(request_frame, sizeof(request_frame));
    if (written != (int)sizeof(request_frame)) {
        ESP_LOGE(TAG, "RS485 write failed (%d/%d)", written, (int)sizeof(request_frame));
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(uart_wait_tx_done(UART485_PORT, pdMS_TO_TICKS(1000)), TAG, "uart_wait_tx_done failed");

    int total_read = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t max_wait = pdMS_TO_TICKS(700);

    while (total_read < (int)response_size && (xTaskGetTickCount() - start) < max_wait) {
        int chunk = uart485_read(response + total_read, response_size - total_read, pdMS_TO_TICKS(60));
        if (chunk < 0) {
            ESP_LOGE(TAG, "RS485 read failed");
            return ESP_FAIL;
        }
        if (chunk == 0) {
            // No bytes in this slice; keep waiting until timeout.
            continue;
        }

        total_read += chunk;
        if (total_read >= MODBUS_RSP_TOTAL_LEN) {
            break;
        }
    }

    *response_len = total_read;

    if (total_read < MODBUS_RSP_TOTAL_LEN) {
        ESP_LOGW(TAG, "Short Modbus response: got %d bytes, need %d", total_read, MODBUS_RSP_TOTAL_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    if (response[0] != request_frame[0] || response[1] != request_frame[1]) {
        ESP_LOGW(TAG, "Unexpected Modbus header: %02X %02X", response[0], response[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (response[2] != MODBUS_RSP_DATA_LEN) {
        ESP_LOGW(TAG, "Unexpected Modbus byte count: %u", response[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static float get_do_value_to_send(void)
{
    // ESP_LOGI(TAG, "RTC DO value: %.2f", (double)s_do_value_rtc);
    return s_do_value_rtc;
}

static bool build_telemetry_json(float do_level, char *json_out, size_t json_out_size)
{
    int written = snprintf(json_out, json_out_size,
                           "{\"cmdarr\":[{\"cmd\":\"DOEX\",\"poolid\":%u,\"unitid\":%u,\"DOLevel\":%.2f,\"DOCount\":%d,\"DOretry\":%d,\"PHLevel\":%.2f,\"IRLevel\":%.2f,\"SALevel\":%.2f,\"WaterTemp\":%.2f,\"Interval\":%d}]}",
                           (unsigned)theConf.poolid, (unsigned)theConf.unitid,
                           (double)do_level, s_count, s_retry_count, 1.0, 2.0, 3.0, waterTemp, (int)theConf.interval);
    // int written = snprintf(json_out, json_out_size,
    //                        "{\"cmdarr\":[{\"cmd\":\"DOEX\",\"poolid\":%u,\"unitid\":%u,\"DOLevel\":%.2f,\"DOCount\":%d,\"DOretry\":%d,\"PHLevel\":%.2f,\"IRLevel\":%.2f,\"SALevel\":%.2f,\"WaterTemp\":%.2f,\"Interval\":%d},\
    //                        {\"cmd\":\"DOPH\",\"poolid\":%u,\"unitid\":%u,\"DOLevel\":%.2f,\"DOCount\":%d,\"DOretry\":%d,\"PHLevel\":%.2f,\"IRLevel\":%.2f,\"SALevel\":%.2f,\"WaterTemp\":%.2f,\"Interval\":%d}]}",
    //                        (unsigned)theConf.poolid, (unsigned)theConf.unitid,
    //                        (double)do_level, s_count, s_retry_count, 1.0, 2.0, 3.0, waterTemp, (int)theConf.interval, (unsigned)theConf.poolid, (unsigned)theConf.unitid,
    //                        (double)do_level, s_count, s_retry_count, 1.0, 2.0, 3.0, waterTemp, (int)theConf.interval);
    return written > 0 && (size_t)written < json_out_size;
}

static bool get_sta_ifreq(struct ifreq *ifr)
{
    if (ifr == NULL || s_sta_netif == NULL) {
        return false;
    }

    memset(ifr, 0, sizeof(*ifr));
    esp_err_t err = esp_netif_get_netif_impl_name(s_sta_netif, ifr->ifr_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get STA netif name: %s", esp_err_to_name(err));
        return false;
    }

    // ESP_LOGI(TAG, "Binding HTTP client to STA interface: %s", ifr->ifr_name);
    return true;
}

static void enter_deep_sleep(const char *who, uint32_t howmuch)
 {
    if(confFlag)
    {
        ESP_LOGI(TAG, "Configuration mode - skipping deep sleep");
        return;
    }
    ESP_LOGI(TAG, "Entering deep sleep[%s] for %d ms count %d retries %d", who, howmuch, s_count, s_retry_count);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)howmuch * 1000ULL));
    set_sleep_rs485();      //crutial for power savings
    esp_deep_sleep_start();
}


static void send_http_post(const char *http_body)
{
    struct ifreq sta_ifr;

    if (!get_sta_ifreq(&sta_ifr)) {
        ESP_LOGE(TAG, "STA interface is not ready, skip HTTP POST");
        return;
    }

    esp_http_client_config_t config = {
        .url = HTTP_POST_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .if_name = &sta_ifr,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return;
    }

    // ESP_LOGI(TAG, "HTTP POST URL: %s", HTTP_POST_URL);

    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-Type", "application/json"));
    ESP_ERROR_CHECK(esp_http_client_set_post_field(client, http_body, strlen(http_body)));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        // ESP_LOGI(TAG, "HTTP POST status=%d, content_length=%d", status_code, content_length);
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void restart_from_mqtt_command(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static bool mqtt_cmd_name_from_json(cJSON *json, char *cmd_out, size_t cmd_out_size)
{
    if (json == NULL || cmd_out == NULL || cmd_out_size == 0) {
        return false;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(json, "cmd");
    if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL) {
        return false;
    }

    int written = snprintf(cmd_out, cmd_out_size, "%s", cmd_item->valuestring);
    return written > 0 && (size_t)written < cmd_out_size;
}

static void handle_mqtt_interval_command(cJSON *json)
{
    cJSON *interval_item = cJSON_GetObjectItem(json, "interval");
    if (!cJSON_IsNumber(interval_item) || interval_item->valueint <= 0) {
        ESP_LOGW(TAG, "MQTT cmd: INTERVAL missing or invalid 'interval' field");
        return;
    }

    theConf.interval = (uint32_t)interval_item->valueint;
    ESP_LOGI(TAG, "MQTT cmd: Set interval to %u seconds", theConf.interval);
    save_theconf_to_nvs();
    restart_from_mqtt_command();
}

static void handle_mqtt_network_command(cJSON *json)
{
    cJSON *network_item = cJSON_GetObjectItem(json, "ssid");
    if (!cJSON_IsString(network_item) || network_item->valuestring == NULL) {
        ESP_LOGW(TAG, "MQTT cmd: SSID missing or invalid 'ssid' field");
        return;
    }

    strncpy(theConf.sta_ssid, network_item->valuestring, sizeof(theConf.sta_ssid) - 1);
    theConf.sta_ssid[sizeof(theConf.sta_ssid) - 1] = '\0';

    cJSON *password_item = cJSON_GetObjectItem(json, "pass");
    if (!cJSON_IsString(password_item) || password_item->valuestring == NULL) {
        ESP_LOGW(TAG, "MQTT cmd: PASSWORD missing or invalid 'pass' field");
        return;
    }

    strncpy(theConf.sta_pass, password_item->valuestring, sizeof(theConf.sta_pass) - 1);
    theConf.sta_pass[sizeof(theConf.sta_pass) - 1] = '\0';

    save_theconf_to_nvs();
    restart_from_mqtt_command();
}

static void dispatch_mqtt_command(const char *cmd, cJSON *json)
{

    if (strcmp(cmd, "configDO") == 0) {
        ESP_LOGW(TAG, "MQTT cmd: CONFIG - entering configuration mode");
        confFlag = true;
        s_config_mode_latched = true;
        save_theconf_to_nvs();  // persist the latch so it survives the next deep-sleep wakeup
        // AP is already up (APSTA mode); kick off the config-mode tasks directly.
        s_ap_client_connected = false;
        start_task_once(&ap_assigned_ip_blink_task, "APBlink", 2048, 4, &s_ap_blink_task);
        start_task_once(&sensor_webserver, "WEB485", 1024 * 10, 5, &s_web_task);
        return;
    }

    if (strcmp(cmd, "intervalDO") == 0) {
        ESP_LOGW(TAG, "MQTT cmd: INTERVAL");
        handle_mqtt_interval_command(json);
        return;
    }

    if (strcmp(cmd, "netwDO") == 0) {
        ESP_LOGW(TAG, "MQTT cmd: NETW");
        handle_mqtt_network_command(json);
        return;
    }

    ESP_LOGW(TAG, "MQTT cmd: unknown command '%s'", cmd);
}

static void clear_retained_mqtt_message(esp_mqtt_client_handle_t client, const char *topic, int topic_len)
{
    if (client == NULL || topic == NULL || topic_len <= 0) {
        ESP_LOGW(TAG, "Cannot clear retained MQTT cmd message: invalid topic/client");
        return;
    }

    char retained_topic[80] = {0};
    size_t copy_len = (size_t)topic_len;
    if (copy_len >= sizeof(retained_topic)) {
        ESP_LOGW(TAG, "MQTT cmd topic too long to clear retained message");
        return;
    }

    memcpy(retained_topic, topic, copy_len);
    retained_topic[copy_len] = '\0';

    int msg_id = esp_mqtt_client_publish(client, retained_topic, "", 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to clear retained MQTT cmd topic: %s", retained_topic);
        return;
    }

    ESP_LOGI(TAG, "Cleared retained MQTT cmd topic: %s (msg_id=%d)", retained_topic, msg_id);
}

static void handle_mqtt_cmd_payload(const char *topic, int topic_len, const char *data, int data_len)
{
    char command[20] = {0};

    if (data == NULL || data_len <= 0) {
        ESP_LOGW(TAG, "MQTT cmd payload is empty");
        return;
    }

    cJSON *json = cJSON_ParseWithLength(data, data_len);
    if(!json) {
        ESP_LOGW(TAG, "MQTT cmd payload is not valid JSON topic=%.*s payload=%.*s", topic_len, topic, data_len, data);
        return;
    }

    if (!mqtt_cmd_name_from_json(json, command, sizeof(command))) {
        ESP_LOGW(TAG, "MQTT cmd payload missing 'cmd' field topic=%.*s payload=%.*s", topic_len, topic, data_len, data);
        cJSON_Delete(json);
        return;
    }

    ESP_LOGI(TAG, "Received MQTT cmd: %s topic=%.*s payload=%.*s", command, topic_len, topic, data_len, data);
    dispatch_mqtt_command(command, json);
    cJSON_Delete(json);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    if (s_mqtt_event_group == NULL) {
        return;
    }

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event == NULL) {
        return;
    }

    if (event_id == MQTT_EVENT_CONNECTED) {
        char cmd_topic[80];
        int topic_len = snprintf(cmd_topic, sizeof(cmd_topic), "shrimpDO/%u/%u/cmd",
                                 (unsigned)theConf.poolid, (unsigned)theConf.unitid);
        if (topic_len > 0 && topic_len < (int)sizeof(cmd_topic)) {
            int sub_id = esp_mqtt_client_subscribe(event->client, cmd_topic, 1);
            if (sub_id < 0) {
                ESP_LOGE(TAG, "MQTT subscribe failed for topic: %s", cmd_topic);
            } else {
                ESP_LOGI(TAG, "Subscribed MQTT topic: %s (msg_id=%d)", cmd_topic, sub_id);
            }
        } else {
            ESP_LOGE(TAG, "MQTT cmd topic build failed for pool=%u unit=%u",
                     (unsigned)theConf.poolid, (unsigned)theConf.unitid);
        }
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
    } else if (event_id == MQTT_EVENT_PUBLISHED) {
        xEventGroupSetBits(s_mqtt_event_group, MQTT_PUBLISHED_BIT);
    } else if (event_id == MQTT_EVENT_DATA) {
        if (event->data != NULL && event->data_len > 0) {
            clear_retained_mqtt_message(event->client, event->topic, event->topic_len);
            handle_mqtt_cmd_payload(event->topic, event->topic_len, event->data, event->data_len);
        } else {
            ESP_LOGW(TAG, "MQTT_EVENT_DATA without payload");
        }
    }
}

static void send_mqtt_publish(const char *payload)
{
    char mqtt_topic[64];
    int topic_len = snprintf(mqtt_topic, sizeof(mqtt_topic), "shrimpDO/%u/%u",
                             (unsigned)theConf.poolid, (unsigned)theConf.unitid);
    if (topic_len <= 0 || topic_len >= (int)sizeof(mqtt_topic)) {
        ESP_LOGE(TAG, "MQTT topic build failed for pool=%u unit=%u",
                 (unsigned)theConf.poolid, (unsigned)theConf.unitid);
        return;
    }

    s_mqtt_event_group = xEventGroupCreate();
    if (s_mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT event group");
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = MQTT_BROKER_URI,
            },
        },
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        vEventGroupDelete(s_mqtt_event_group);
        s_mqtt_event_group = NULL;
        return;
    }

    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        vEventGroupDelete(s_mqtt_event_group);
        s_mqtt_event_group = NULL;
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_mqtt_event_group,
        MQTT_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));

    if ((bits & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "MQTT connect timeout");
    } else {
        ESP_LOGI(TAG, "Publishing MQTT topic: %s", mqtt_topic);
        int msg_id = esp_mqtt_client_publish(client, mqtt_topic, payload, 0, 1, 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "MQTT publish failed");
        } else {
            EventBits_t pub_bits = xEventGroupWaitBits(
                s_mqtt_event_group,
                MQTT_PUBLISHED_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(3000));
            if ((pub_bits & MQTT_PUBLISHED_BIT) == 0) {
                ESP_LOGW(TAG, "MQTT publish did not get ack before timeout");
            }
        }
    }

    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    vEventGroupDelete(s_mqtt_event_group);
    s_mqtt_event_group = NULL;
}

static void send_telemetry_message(void)
{
    char payload[HTTP_POST_BODY_MAX_LEN];
    float do_level = get_do_value_to_send();
    if (!build_telemetry_json(do_level, payload, sizeof(payload))) {
        ESP_LOGE(TAG, "Failed to build telemetry JSON payload");
        return;
    }
    if (s_transport_mode == MESSAGE_TRANSPORT_MQTT) {
        send_mqtt_publish(payload);
    } else {
        send_http_post(payload);
    }
}

static esp_err_t configure_softap_subnet(void)
{
    if (s_ap_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
// cannot have 192.168.4.1 as standard. Overlapping with our shrimp node AP  so use 5 instead of 4

    esp_netif_ip_info_t ap_ip_info = {};
    IP4_ADDR(&ap_ip_info.ip, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip_info.gw, 192, 168, 5, 1);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);

    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Failed to stop SoftAP DHCP server: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ap_ip_info), TAG, "set SoftAP ip failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "start SoftAP DHCP server failed");

    // ESP_LOGI(TAG, "SoftAP subnet set to " IPSTR "/24 (gw " IPSTR ")",
    //          IP2STR(&ap_ip_info.ip), IP2STR(&ap_ip_info.gw));
    return ESP_OK;
}

static void ap_assigned_ip_blink_task(void *pvParameters)
{
    (void)pvParameters;
    int level = 0;

    ESP_LOGI(TAG, "Started NTW blink task after AP client IP assignment");
    while (true) {
        uint32_t blink_ms = confFlag ? (s_ap_client_connected ? 100 : 300) : 200;
        level = !level;
        gpio_set_level(NTW, level);
        vTaskDelay(pdMS_TO_TICKS(blink_ms));
    }
}

static void collect_do_sample_until_ready(void)
{
    uint8_t rs485_response[UART485_RX_BUF_SIZE] = {0};
    int retries = 0;
    int rs485_response_len = 0;

    while (true) {
        esp_err_t rs485_err = rs485_send_read_do_request(rs485_response, sizeof(rs485_response), &rs485_response_len);
        if (rs485_err == ESP_OK && rs485_response_len > 0) {
            // ESP_LOGI(TAG, "RS485 response length: %d", rs485_response_len);
            // ESP_LOG_BUFFER_HEX(TAG, rs485_response, rs485_response_len);

            DOHandler(rs485_response + MODBUS_RSP_DATA_OFFSET, MODBUS_RSP_DATA_LEN);

            if (s_do_value_rtc > MINDO) {
                // Valid DO value: send immediately and skip additional averaging.
                break;
            }

            avgDO += s_do_value_rtc;
            vTaskDelay(pdMS_TO_TICKS(WAITDO));
            retries++;
            s_retry_count++;
            if (retries >= MAXRETRY_MAX485) {
                ESP_LOGI(TAG, "Retrys exhausted");
                s_do_value_rtc = avgDO / retries;
                break;
            }
        } else {
            ESP_LOGE(TAG, "RS485 request failed: %s %d", esp_err_to_name(rs485_err), rs485_response_len);
            vTaskDelay(pdMS_TO_TICKS(1000)); // Short delay before retrying
        }
    }
}

static void publish_cycle_and_update_state(void)
{
    if (s_message_sent) {
        return;
    }

    s_message_sent = true;
    send_telemetry_message();
    theConf.lifecount = s_count;
    theConf.retrycount = s_retry_count;
    s_count = theConf.lifecount;
    if (theConf.lifecount % 3 == 0) {
        save_theconf_to_nvs(); // every 3
    }
    enter_deep_sleep("SYS", DEEP_SLEEP_MS*theConf.interval);
}

void rs485_task_manager(void *pvParameters)
{
    (void)pvParameters;
    while (true) {
        collect_do_sample_until_ready();
        publish_cycle_and_update_state();

        // Delay before next cycle; in non-config mode deep sleep will normally restart the device first.
        vTaskDelay(pdMS_TO_TICKS(theConf.interval * 60000));
        s_message_sent = false;
    }
} // task will be killed due to sleep, no need to delete it explicitly or crash will not happen

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_AP_START) {
            // ESP_LOGI(TAG, "SoftAP started, SSID=%s", AP_SSID);
            if (confFlag) {
                s_ap_client_connected = false;
                start_task_once(&ap_assigned_ip_blink_task, "APBlink", 2048, 4, &s_ap_blink_task);
            }
            if (confFlag && s_web_task == NULL) {
                start_task_once(&sensor_webserver, "WEB485", 1024 * 10, 5, &s_web_task);
            }
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            if (confFlag) {
                s_ap_client_connected = false;
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (confFlag) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                ESP_LOGI(TAG, "STA disconnected, reconnecting...");
                enter_deep_sleep("WiFi",DEEP_SLEEP_MS_WIFI);         // better save energy when wifi is not available, instead of retrying every 10 seconds
                esp_wifi_connect();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        // ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        if (s_ap_netif != NULL) {
            esp_netif_ip_info_t ap_ip_info = {};
            if (esp_netif_get_ip_info(s_ap_netif, &ap_ip_info) == ESP_OK) {
                // ESP_LOGI(TAG, "AP  IP: " IPSTR " GW: " IPSTR " NM: " IPSTR,
                //          IP2STR(&ap_ip_info.ip),
                //          IP2STR(&ap_ip_info.gw),
                //          IP2STR(&ap_ip_info.netmask));
            }
        }
        if (!s_theconf_invalid && s_rs485_task == NULL) {
            start_task_once(&rs485_task_manager, "RS485", 1024 * 10, 5, &s_rs485_task);
        }
        if (confFlag) {
            ESP_LOGW(TAG, "Configuration Mode");
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
        // ESP_LOGI(TAG, "AP assigned client IP: " IPSTR, IP2STR(&event->ip));
        s_ap_client_connected = true;

        start_task_once(&ap_assigned_ip_blink_task, "APBlink", 2048, 4, &s_ap_blink_task);

    }
}

void start_network()
{

    if (s_transport_mode == MESSAGE_TRANSPORT_MQTT) {
        snprintf(STA_SSID, sizeof(STA_SSID), "%s", mqtt_sta_ssid_from_conf());
    } else {
        snprintf(STA_SSID, sizeof(STA_SSID), "shrimp-p-%03u-u-%02u", theConf.poolid, theConf.unitid);
    }
    snprintf(AP_SSID, sizeof(AP_SSID), "DOSensor-%03u-%02u", theConf.poolid, theConf.unitid);
    DEEP_SLEEP_MS=MINTIME; // update deep sleep time according to configuration, default  minutes (60000 ms)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(s_sta_netif ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(s_ap_netif ? ESP_OK : ESP_FAIL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t sta_config = {};
    wifi_config_t ap_config = {};
    copy_cstr_to_u8(sta_config.sta.ssid, sizeof(sta_config.sta.ssid), STA_SSID);

    const char *sta_pass = (s_transport_mode == MESSAGE_TRANSPORT_MQTT) ?
                           mqtt_sta_pass_from_conf() : STA_PASS;
    copy_cstr_to_u8(sta_config.sta.password, sizeof(sta_config.sta.password), sta_pass);

    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;
    copy_cstr_to_u8(ap_config.ap.ssid, sizeof(ap_config.ap.ssid), AP_SSID);
    copy_cstr_to_u8(ap_config.ap.password, sizeof(ap_config.ap.password), AP_PASS);
    size_t ap_ssid_len = strnlen((const char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
    size_t ap_pass_len = strnlen((const char *)ap_config.ap.password, sizeof(ap_config.ap.password));

    ap_config.ap.ssid_len = ap_ssid_len;
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = (ap_pass_len >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.beacon_interval = 100;

    if (s_theconf_invalid) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    } else {
#if WIFI_USE_APSTA
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
#else
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
#if WIFI_USE_APSTA
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
#endif
        ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                                              WIFI_PROTOCOL_11B));
        esp_wifi_internal_set_fix_rate(WIFI_IF_STA, true,WIFI_PHY_RATE_1M_L);        // for max distance, set to 1Mbps
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    // Some targets/drivers reject high values depending on country/regulatory limits.
    esp_err_t txpwr_err = esp_wifi_set_max_tx_power(78); // 78 ~= 19.5 dBm
    if (txpwr_err != ESP_OK) {
        ESP_LOGW(TAG, "set_max_tx_power(78) failed: %s; trying 60", esp_err_to_name(txpwr_err));
        txpwr_err = esp_wifi_set_max_tx_power(60);
        if (txpwr_err != ESP_OK) {
            ESP_LOGW(TAG, "set_max_tx_power(60) failed: %s", esp_err_to_name(txpwr_err));
        }
    }

    esp_err_t ap_subnet_err = configure_softap_subnet();
    if (ap_subnet_err != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP subnet reconfigure failed: %s", esp_err_to_name(ap_subnet_err));
    }

    if (s_theconf_invalid) {
        ESP_LOGI(TAG, "Wi-Fi AP-only configuration mode started. AP SSID:%s", AP_SSID);
    } 
}

extern "C"{
void app_main(void)
{
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    gpio_config_t conf_btn = {};
    conf_btn.pin_bit_mask = (1ULL << BOOT_BTN_GPIO);
    conf_btn.mode = GPIO_MODE_INPUT;
    conf_btn.pull_up_en = GPIO_PULLUP_ENABLE;
    conf_btn.pull_down_en = GPIO_PULLDOWN_DISABLE;
    conf_btn.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&conf_btn));


    // init the rs485 uart and gpio for DE/RE control
    ESP_ERROR_CHECK(uart485_init());
    #ifdef SLEEP
    ESP_ERROR_CHECK(app_gpio_outputs_init());
    #endif

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_err_t conf_err = load_theconf_from_nvs();
    if (conf_err != ESP_OK) {
        ESP_LOGW(TAG, "Continuing with in-memory theConf defaults");
    }
    load_transport_mode_from_conf();
    ESP_LOGI(TAG, "Transport from theConf.conntype: %d (%s)",
             s_transport_mode, transport_mode_str(s_transport_mode));

    if (s_theconf_invalid) {
        ESP_LOGW(TAG, "Invalid/legacy theConf detected, forcing AP-only configuration mode");
    }

    // Decide mode from GPIO0 after loading persisted configuration.
    select_boot_mode_and_transport(wakeup_reason);

    if (confFlag) {
        blink_mode_indicator(2);
    } else if (s_transport_mode == MESSAGE_TRANSPORT_MQTT) {
        blink_mode_indicator(3);
    }

    if (!woke_from_deep_sleep(wakeup_reason)) {
    s_count = theConf.lifecount; // Optional: reset counter on first boot
    s_retry_count=theConf.retrycount;
  }
    start_network();
}
}