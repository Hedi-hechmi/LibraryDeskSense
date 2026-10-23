#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "esp_adc/adc_oneshot.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "coap3/coap.h"
#include "lwip/sockets.h"

// gpio pin definitions
#define TRIG_PIN 5
#define ECHO_PIN 18
#define LDR_CHANNEL ADC_CHANNEL_6 // GPIO34
#define MIC_CHANNEL ADC_CHANNEL_7 // GPIO35

// sampling rates (in ms)
#define OCCUPANCY_PERIOD_MS 1000
#define NOISE_PERIOD_MS 500
#define LIGHT_PERIOD_MS 2000
#define EVENT_PERIOD_MS 1000

// event thresholds ( now variavle can be updated via MQTT )
int occupancy_thr_cm = 50;
int noise_thr = 200;
int light_thr = 1000;
int sampling_rate_ms = 1000; // sampling rate for event detection
// communication mode: "mqtt", "http" or  "CoAP"
char communication_mode[16] = "mqtt"; // default mode


// number of samples for noise measurement
#define NOISE_SAMPLES 100

// Wifi credentials
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

static const char *TAG_WIFI = "WIFI";

// Server adresses 
#define MQTT_BROKER_URI "mqtt://172.20.10.12:1883"
#define HTTP_TELEMETRY_URI "http://172.20.10.12:8080/telemetry"
#define COAP_TELEMETRY_URI "coap://172.20.10.12:5683/telemetry"

// MQTT topics
#define TOPIC_TELEMETRY       "desk/telemetry"
#define TOPIC_EVENT_OCCUPIED  "desk/events/occupied"
#define TOPIC_EVENT_RELEASED  "desk/events/released"
#define TOPIC_EVENT_NOISE     "desk/events/high_noise"
#define TOPIC_EVENT_LIGHT     "desk/events/poor_lighting"
// MQTT config topic (subscribe)
#define TOPIC_CONFIG_SAMPLING "desk/config/sampling_rate"
#define TOPIC_CONFIG_NOISE_THR "desk/config/noise_thr"
#define TOPIC_CONFIG_LIGHT_THR "desk/config/light_thr"
#define TOPIC_CONFIG_OCCUPANCY "desk/config/occupancy_thr"
#define TOPIC_CONFIG_MODE       "desk/config/communication_mode"
static const char *TAG_MQTT = "MQTT";

// Global MQTT client handle
esp_mqtt_client_handle_t mqtt_client = NULL;

// Connection status flag
bool mqtt_connected = false;

// shared data
float distance_cm = -1;
int noise_level = 0;
int light_level = 0;
bool desk_occupied = false;

//Mutex to protect shared data
SemaphoreHandle_t data_mutex;
SemaphoreHandle_t adc_mutex;

// ADC handle 
adc_oneshot_unit_handle_t adc1_handle;

// measure distance with HC-SR04 
float read_ultrasonic_cm(void)
{
    gpio_set_level(TRIG_PIN, 0);
    ets_delay_us(2);
    gpio_set_level(TRIG_PIN, 1);
    ets_delay_us(10);
    gpio_set_level(TRIG_PIN, 0);

    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 0) {
        if (esp_timer_get_time() - start_time > 30000) {
            return -1.0;
        }
    }

    int64_t echo_start = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 1) {
        if (esp_timer_get_time() - echo_start > 30000) {
            return -1.0;
        }
    }
    int64_t echo_end = esp_timer_get_time();

    int64_t duration = echo_end - echo_start;
    return (duration * 0.0343) / 2.0;
}
// HTTP: send telemetry payload via POST
static const char *TAG_HTTP = "HTTP";

void send_http_telemetry(const char *payload)
{
    esp_http_client_config_t config = {
        .url = HTTP_TELEMETRY_URI,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG_HTTP, "POST status: %d", status);
    } else {
        ESP_LOGE(TAG_HTTP, "POST failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}
// CoAP: send telemetry payload via POST
static const char *TAG_COAP = "COAP";

void send_coap_telemetry(const char *payload)
{
    coap_context_t *ctx = NULL;
    coap_session_t *session = NULL;
    coap_address_t dst_addr;
    coap_uri_t uri;
    coap_pdu_t *pdu = NULL;

    // Parse the URI
    if (coap_split_uri((const uint8_t *)COAP_TELEMETRY_URI,
                        strlen(COAP_TELEMETRY_URI), &uri) < 0) {
        ESP_LOGE(TAG_COAP, "Failed to parse URI");
        return;
    }

    // Set destination address
    coap_address_init(&dst_addr);
    dst_addr.addr.sin.sin_family = AF_INET;
    dst_addr.addr.sin.sin_port = htons(uri.port);
    inet_pton(AF_INET, "172.20.10.12", &dst_addr.addr.sin.sin_addr);
    dst_addr.size = sizeof(dst_addr.addr.sin);

    // Create CoAP context
    ctx = coap_new_context(NULL);
    if (!ctx) {
        ESP_LOGE(TAG_COAP, "Failed to create context");
        return;
    }

    // Create a session to the server (UDP, no encryption)
    session = coap_new_client_session(ctx, NULL, &dst_addr, COAP_PROTO_UDP);
    if (!session) {
        ESP_LOGE(TAG_COAP, "Failed to create session");
        coap_free_context(ctx);
        return;
    }

    // Build the POST request (non-confirmable = fire-and-forget)
    pdu = coap_new_pdu(COAP_MESSAGE_NON, COAP_REQUEST_CODE_POST, session);
    if (!pdu) {
        ESP_LOGE(TAG_COAP, "Failed to create PDU");
        coap_session_release(session);
        coap_free_context(ctx);
        return;
    }

    // Add the URI path option
    coap_add_option(pdu, COAP_OPTION_URI_PATH, uri.path.length, uri.path.s);

    // Add the payload (the JSON string)
    coap_add_data(pdu, strlen(payload), (const uint8_t *)payload);

    // Send the request
    coap_send(session, pdu);

    // Process I/O for up to 1 second
    coap_io_process(ctx, 1000);

    ESP_LOGI(TAG_COAP, "Sent CoAP POST to %s", COAP_TELEMETRY_URI);

    // Cleanup
    coap_session_release(session);
    coap_free_context(ctx);
}
// Task 1: Occupancy (ultrasonic)
void occupancy_task(void *param)
{
    // Configure GPIO pins for the ultrasonic sensor
    gpio_reset_pin(TRIG_PIN);
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(ECHO_PIN);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);

    while (1) {
        float dist = read_ultrasonic_cm();

        // Lock the shared data, update it, unlock
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        distance_cm = dist;
        desk_occupied = (dist > 0 && dist < occupancy_thr_cm);
        xSemaphoreGive(data_mutex);

        vTaskDelay(pdMS_TO_TICKS(OCCUPANCY_PERIOD_MS));
    }
}
// Helper: measure noise level (peak-to-peak)
int read_noise_level(void)
{
    int peak = 0;
    int min  = 4095;
    xSemaphoreTake(adc_mutex, portMAX_DELAY);
    for (int i = 0; i < NOISE_SAMPLES; i++) {
        int sample;
        adc_oneshot_read(adc1_handle, MIC_CHANNEL, &sample);
        if (sample > peak) peak = sample;
        if (sample < min)  min  = sample;
    }
    xSemaphoreGive(adc_mutex);
    return peak - min;
}

// Task 2: Noise (sound sensor)
void noise_task(void *param)
{
    while (1) {
        int noise = read_noise_level();

        xSemaphoreTake(data_mutex, portMAX_DELAY);
        noise_level = noise;
        xSemaphoreGive(data_mutex);

        vTaskDelay(pdMS_TO_TICKS(NOISE_PERIOD_MS));
    }
}
// Task 3: Light (LDR)
void light_task(void *param)
{
    while (1) {
        int raw;
        xSemaphoreTake(adc_mutex, portMAX_DELAY);
        adc_oneshot_read(adc1_handle, LDR_CHANNEL, &raw);
        xSemaphoreGive(adc_mutex);

        xSemaphoreTake(data_mutex, portMAX_DELAY);
        light_level = raw;
        xSemaphoreGive(data_mutex);

        vTaskDelay(pdMS_TO_TICKS(LIGHT_PERIOD_MS));
    }
}
// Task 4: Event detector
void event_task(void *param)
{
    // Local copies (to minimize time holding the mutex)
    float local_distance;
    int   local_noise;
    int   local_light;
    bool  local_occupied;
    bool was_occupied = false;

    char payload[128];

    while (1) {
        // Take a snapshot of shared data 
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        local_distance = distance_cm;
        local_noise    = noise_level;
        local_light    = light_level;
        local_occupied = desk_occupied;
        xSemaphoreGive(data_mutex);

        // Print general status
        printf("[STATUS] dist=%.1fcm  noise=%d  light=%d  occupied=%s\n",
               local_distance, local_noise, local_light,
               local_occupied ? "YES" : "NO");

        // Build the JSON payload once
snprintf(payload, sizeof(payload),
    "{\"dist\":%.1f,\"noise\":%d,\"light\":%d,\"occupied\":%s}",
    local_distance, local_noise, local_light,
    local_occupied ? "true" : "false");

// Send telemetry based on selected communication mode
if (strcmp(communication_mode, "mqtt") == 0) {
    if (mqtt_connected) {
        esp_mqtt_client_publish(mqtt_client, TOPIC_TELEMETRY, payload, 0, 1, 0);
    }
} else if (strcmp(communication_mode, "http") == 0) {
    send_http_telemetry(payload);
} else if (strcmp(communication_mode, "coap") == 0) {
    send_coap_telemetry(payload);
}


        // Detect events and publish to MQTT if connected
        if (local_occupied && !was_occupied) {
            printf("  >> EVENT: desk_occupied 🪑\n");
            if (mqtt_connected) {
                esp_mqtt_client_publish(mqtt_client, TOPIC_EVENT_OCCUPIED, "desk_occupied", 0, 1, 0);
            }
        }
        if (!local_occupied && was_occupied) {
            printf("  >> EVENT: desk_released ✅\n");
            if (mqtt_connected) {
                esp_mqtt_client_publish(mqtt_client, TOPIC_EVENT_RELEASED, "desk_released", 0, 1, 0);
            }
        }
        if (local_noise > noise_thr) {
            printf("  >> EVENT: high_noise_event 🔔 (noise=%d)\n", local_noise);
            if (mqtt_connected) {
                snprintf(payload, sizeof(payload), "{\"noise\":%d}", local_noise);
                esp_mqtt_client_publish(mqtt_client, TOPIC_EVENT_NOISE, payload, 0, 1, 0);
            }
        }
        if (local_light < light_thr) {
            printf("  >> EVENT: poor_lighting_event 💡 (light=%d)\n", local_light);
            if (mqtt_connected) {
                snprintf(payload, sizeof(payload), "{\"light\":%d}", local_light);
                esp_mqtt_client_publish(mqtt_client, TOPIC_EVENT_LIGHT, payload, 0, 1, 0);
            }
        }

        was_occupied = local_occupied;
        vTaskDelay(pdMS_TO_TICKS(sampling_rate_ms));
    }
}
// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_WIFI, "WiFi started, connecting...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG_WIFI, "Disconnected. Reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_WIFI, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// WiFi initialization
void wifi_init(void)
{
    // Initialize NVS (required for WiFi to save credentials)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Create the default event loop
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    // Initialize the WiFi driver with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Register event handlers
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    // Configure WiFi as a station (client connecting to a router)
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);  

    ESP_LOGI(TAG_WIFI, "WiFi init done. SSID: %s", WIFI_SSID);
}

    // MQTT event handler 
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_MQTT, "Connected to broker!");
            mqtt_connected = true;
            // Subscribe to config topics
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONFIG_SAMPLING, 1);
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONFIG_NOISE_THR, 1);
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONFIG_LIGHT_THR, 1);
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONFIG_OCCUPANCY, 1);
            esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONFIG_MODE, 1);
            ESP_LOGI(TAG_MQTT, "Subscribed to config topics.");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG_MQTT, "Disconnected from broker.");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_DATA: {     
            // a message arrived on a topic we subscribed to
            char topic[64] = {0};
            char data[64] = {0};
            int tlen = (event->topic_len < 63 ) ? event->topic_len : 63;
            int dlen = (event->data_len < 63) ? event->data_len : 63;
            memcpy(topic, event->topic, tlen);
            memcpy(data, event->data, dlen);
            int value = atoi(data); // convert data to integer

            if (strcmp(topic, TOPIC_CONFIG_SAMPLING) == 0) {
                sampling_rate_ms = value;
                ESP_LOGI(TAG_MQTT, "Updated sampling_rate_ms to %d", value);
            } 
            else if (strcmp(topic, TOPIC_CONFIG_NOISE_THR) == 0) {
                noise_thr = value;
                ESP_LOGI(TAG_MQTT, "Updated noise_thr to %d", value);
            } 
            else if (strcmp(topic, TOPIC_CONFIG_LIGHT_THR) == 0) {
                light_thr = value;
                ESP_LOGI(TAG_MQTT, "Updated light_thr to %d", value);
            } 
            else if (strcmp(topic, TOPIC_CONFIG_OCCUPANCY) == 0) {
                occupancy_thr_cm = value;
                ESP_LOGI(TAG_MQTT, "Updated occupancy_thr_cm to %d", value);
            } 
            else if (strcmp(topic, TOPIC_CONFIG_MODE) == 0) {
                strncpy(communication_mode, data, sizeof(communication_mode) - 1);
                communication_mode[sizeof(communication_mode) - 1] = '\0'; // ensure null-termination
                ESP_LOGI(TAG_MQTT, "Communication mode set to: %s", communication_mode);
            } 
            break;
        }
        case MQTT_EVENT_ERROR: 
            ESP_LOGE(TAG_MQTT, "MQTT error occurred.");
            break;
        default:
            break;
    }
}

// MQTT initialization
void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI(TAG_MQTT, "MQTT init done. Broker: %s", MQTT_BROKER_URI);
}
void app_main(void)
{
    wifi_init();
    // wait for WiFi to connect before starting MQTT
    vTaskDelay(pdMS_TO_TICKS(5000));
    mqtt_init();
    // Create the mutex 
    data_mutex = xSemaphoreCreateMutex();
    adc_mutex = xSemaphoreCreateMutex();

    // Initialize ADC1 (shared between noise and light tasks)
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_new_unit(&init_cfg, &adc1_handle);

    // Configure channel 6 (LDR, GPIO 34) 
    adc_oneshot_chan_cfg_t ldr_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(adc1_handle, LDR_CHANNEL, &ldr_cfg);

    // Configure channel 7 (Microphone, GPIO 35) 
    adc_oneshot_chan_cfg_t mic_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(adc1_handle, MIC_CHANNEL, &mic_cfg);

    printf("\n=== LibraryDeskSense starting ===\n\n");

    // Create the 4 tasks 
    xTaskCreate(occupancy_task, "Occupancy", 4096, NULL, 5, NULL);
    xTaskCreate(noise_task,     "Noise",     4096, NULL, 5, NULL);
    xTaskCreate(light_task,     "Light",     4096, NULL, 4, NULL);
    xTaskCreate(event_task,     "Event",     4096, NULL, 6, NULL);
}