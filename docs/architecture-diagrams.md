# APSTA Shrimp Architecture Diagrams

## System Architecture

```mermaid
flowchart LR
    subgraph HW[ESP32-S3 Device Layer]
        Sensor[DO Sensor Modbus/RS485]
        MAX485[MAX485 Transceiver\nDE=GPIO14, RE=GPIO13]
        UART[UART1 9600 Half-Duplex\nTX=GPIO42 RX=GPIO2]
        RTC[RTC Memory\ns_do_value_rtc, counters]
    end

    subgraph FW[Application Firmware]
        BOOT[app_main]
        INIT[Init: UART/GPIO, NVS, netif, event loop]
        WIFI[Wi-Fi AP+STA Mode]
        EVT[wifi_event_handler]
        TASK[rs485_task_manager]
        PARSE[DOHandler parse floats\n(temp, percent, do)]
        JSON[build_http_post_json]
        HTTP[HTTP POST /api/remoteLevels]
        SLEEP[enter_deep_sleep]
    end

    subgraph NET[Network]
        STA[STA uplink to router\nshrimp-p-001-u-01]
        AP[SoftAP DOSensor]
        API[HTTP Server\n192.168.4.1]
    end

    Sensor --> MAX485 --> UART --> TASK
    BOOT --> INIT --> WIFI --> EVT
    WIFI --> STA
    WIFI --> AP
    EVT -->|IP_EVENT_STA_GOT_IP| TASK
    TASK --> PARSE --> RTC --> JSON --> HTTP --> API
    EVT -->|WIFI_EVENT_STA_DISCONNECTED| SLEEP
    TASK -->|success/fallback average| SLEEP
```

## Runtime Flow

```mermaid
flowchart TD
    A[Boot or wake from deep sleep] --> B{Wakeup reason timer?}
    B -->|No| C[Reset sample counter]
    B -->|Yes| D[Keep RTC DO value/counters]
    C --> E[Init UART485 + DE/RE GPIO + NVS + Wi-Fi stack]
    D --> E

    E --> F[Start Wi-Fi APSTA]
    F --> G{STA got IP?}
    G -->|No, disconnect| H[Sleep short DEEP_SLEEP_MS_WIFI]
    H --> A
    G -->|Yes| I[Create rs485_task_manager]

    I --> J[Send Modbus request frame]
    J --> K[Read RS485 response]
    K --> L{Valid response?}
    L -->|No| J
    L -->|Yes| M[Parse DO value]
    M --> N{DO > MINDO?}
    N -->|Yes| O[Use immediate DO]
    N -->|No| P[Accumulate average + retry]
    P --> Q{Retries exhausted?}
    Q -->|No| J
    Q -->|Yes| R[Use average DO]

    O --> S[Build JSON payload]
    R --> S
    S --> T[HTTP POST /api/remoteLevels]
    T --> U[Deep sleep DEEP_SLEEP_MS]
    U --> A
```
