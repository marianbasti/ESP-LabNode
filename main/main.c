#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

#define TAG "temcontrol"

// GPIO configuration from menuconfig
#define DHT_GPIO CONFIG_GPIO_DHT
#define RELAY_GPIO CONFIG_GPIO_RELAY
#define LED_GPIO CONFIG_GPIO_LED

// WiFi event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Add new constant for default hostname
#define DEFAULT_HOSTNAME "temcontrol"
#define NVS_NAMESPACE "storage"
#define NVS_KEY_HOSTNAME "hostname"

// Update these constants
#define MAX_SENSOR_RETRIES 3
#define SENSOR_RETRY_DELAY_MS 2000  // Increased to 2 seconds
#define SENSOR_READ_TIMEOUT_MS 3000

// Add these new defines after other defines
#define DHT_TIMEOUT_US 10000
#define DHT_START_SIGNAL_US 18000
#define DHT_RESPONSE_SIGNAL_US 40

static EventGroupHandle_t wifi_event_group;
static httpd_handle_t server = NULL;

// Timer structure
typedef struct {
    bool enabled;
    uint32_t on_duration;
    uint32_t off_duration;
    uint32_t last_toggle;
    bool current_state;
} timer_config_t;

static timer_config_t relay_timer = {
    .enabled = false,
    .on_duration = 0,
    .off_duration = 0,
    .last_toggle = 0,
    .current_state = false
};

// Function declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
static void initialize_wifi(void);
static esp_err_t sensor_get_handler(httpd_req_t *req);
static esp_err_t relay_post_handler(httpd_req_t *req);
static esp_err_t timer_handler(httpd_req_t *req);
static esp_err_t hostname_get_handler(httpd_req_t *req);
static esp_err_t hostname_post_handler(httpd_req_t *req);
static void timer_control_task(void *pvParameters);

// Initialize GPIO
static void initialize_gpio(void) {
    gpio_reset_pin(RELAY_GPIO);
    gpio_reset_pin(LED_GPIO);
    gpio_reset_pin(DHT_GPIO);
    
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(DHT_GPIO);  // Enable pullup for DHT
    
    gpio_set_level(RELAY_GPIO, 0);
    gpio_set_level(LED_GPIO, 0);
}

// HTTP server configuration
static const httpd_uri_t sensor_uri = {
    .uri       = "/api/sensor",
    .method    = HTTP_GET,
    .handler   = sensor_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t relay_uri = {
    .uri       = "/api/relay",
    .method    = HTTP_POST,
    .handler   = relay_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t timer_uri = {
    .uri       = "/api/timer",
    .method    = HTTP_GET,    // Changed to GET
    .handler   = timer_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t timer_post_uri = {    // Added new URI for POST
    .uri       = "/api/timer",
    .method    = HTTP_POST,
    .handler   = timer_handler,
    .user_ctx  = NULL
};

// Add new URI handlers
static const httpd_uri_t hostname_get_uri = {
    .uri       = "/api/hostname",
    .method    = HTTP_GET,asdfasdfsdkjfgh
    .handler   = hostname_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t hostname_post_uri = {
    .uri       = "/api/hostname",
    .method    = HTTP_POST,
    .handler   = hostname_post_handler,
    .user_ctx  = NULL
};

// Start HTTP server
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &sensor_uri);
        httpd_register_uri_handler(server, &relay_uri);
        httpd_register_uri_handler(server, &timer_uri);        // Register GET handler
        httpd_register_uri_handler(server, &timer_post_uri);   // Register POST handler
        httpd_register_uri_handler(server, &hostname_get_uri);
        httpd_register_uri_handler(server, &hostname_post_uri);
        return server;
    }
    return NULL;
}

// Main application
void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize GPIO
    initialize_gpio();
    
    // Initialize WiFi
    initialize_wifi();
    
    // Start HTTP server
    start_webserver();
    
    // Start timer control task
    xTaskCreate(timer_control_task, "timer_control", 2048, NULL, 5, NULL);
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        gpio_set_level(LED_GPIO, 1); // LED on when disconnected
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        gpio_set_level(LED_GPIO, 0); // LED off when connected
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi
static void initialize_wifi(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Add helper function for JSON string creation
static void create_json_response(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, size, format, args);
    va_end(args);
}

static void dht_send_start_signal(void) {
    gpio_set_direction(DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_GPIO, 0);
    esp_rom_delay_us(DHT_START_SIGNAL_US);
    gpio_set_level(DHT_GPIO, 1);
    esp_rom_delay_us(40);
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);
}

static esp_err_t dht_wait_for_level(int level, int timeout_us) {
    int tries = 0;
    while (gpio_get_level(DHT_GPIO) != level) {
        if (tries++ > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }
    return ESP_OK;
}

static esp_err_t dht_read_byte(uint8_t *data) {
    *data = 0;
    for (int i = 0; i < 8; i++) {
        if (dht_wait_for_level(1, DHT_TIMEOUT_US) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(40); // Wait to see if high pulse is long (1) or short (0)
        *data <<= 1;
        if (gpio_get_level(DHT_GPIO) == 1) {
            *data |= 1;
        }
        if (dht_wait_for_level(0, DHT_TIMEOUT_US) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

// Replace read_sensor_safe with this version
static esp_err_t read_sensor_safe(float *temperature, float *humidity) {
    uint8_t data[5] = {0};
    
    // Quick GPIO check first
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    if (gpio_get_level(DHT_GPIO) == -1) {
        ESP_LOGE(TAG, "GPIO read failed");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Disable interrupts during critical timing
    portDISABLE_INTERRUPTS();
    
    // Send start signal
    dht_send_start_signal();
    
    // Wait for DHT response
    esp_err_t ret = dht_wait_for_level(0, DHT_TIMEOUT_US);
    if (ret != ESP_OK) {
        portENABLE_INTERRUPTS();
        ESP_LOGE(TAG, "No response from sensor");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Wait for DHT to pull up
    ret = dht_wait_for_level(1, DHT_TIMEOUT_US);
    if (ret != ESP_OK) {
        portENABLE_INTERRUPTS();
        return ret;
    }
    
    // Wait for DHT to pull down
    ret = dht_wait_for_level(0, DHT_TIMEOUT_US);
    if (ret != ESP_OK) {
        portENABLE_INTERRUPTS();
        return ret;
    }
    
    // Read 5 bytes
    for (int i = 0; i < 5; i++) {
        ret = dht_read_byte(&data[i]);
        if (ret != ESP_OK) {
            portENABLE_INTERRUPTS();
            ESP_LOGE(TAG, "Failed to read byte %d", i);
            return ret;
        }
    }
    
    portENABLE_INTERRUPTS();
    
    // Verify checksum
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        ESP_LOGE(TAG, "Checksum failed");
        return ESP_ERR_INVALID_CRC;
    }
    
    // Convert the data
    *humidity = data[0] + data[1] * 0.1;
    *temperature = data[2] + data[3] * 0.1;
    
    ESP_LOGI(TAG, "Read success: temp=%.1f, humidity=%.1f", *temperature, *humidity);
    return ESP_OK;
}

// Update the sensor_get_handler to include better error reporting
static esp_err_t sensor_get_handler(httpd_req_t *req) {
    float temperature = 0, humidity = 0;
    esp_err_t ret = read_sensor_safe(&temperature, &humidity);
    
    char response[100];
    if (ret == ESP_OK) {
        create_json_response(response, sizeof(response), 
            "{\"temperature\":%.1f,\"humidity\":%.1f,\"status\":\"ok\"}", 
            temperature, humidity);
    } else {
        const char* error_msg = (ret == ESP_ERR_NOT_FOUND) ? 
            "Sensor not connected" : "Failed to read sensor";
        
        create_json_response(response, sizeof(response), 
            "{\"error\":\"%s\",\"status\":\"error\",\"code\":%d}", 
            error_msg, ret);
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t relay_post_handler(httpd_req_t *req) {
    char buf[100];
    char response[100];
    bool success = false;
    int state = 0;
    
    // Get content length
    size_t recv_size = req->content_len;
    if (recv_size >= sizeof(buf)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    
    // Read payload
    int ret = httpd_req_recv(req, buf, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    
    buf[ret] = '\0';
    
    // Parse and set relay state
    if (strstr(buf, "\"state\":\"on\"")) {
        state = 1;  // Set to ON
        success = true;
    } else if (strstr(buf, "\"state\":\"off\"")) {
        state = 0;  // Set to OFF
        success = true;
    }

    if (success) {
        relay_timer.enabled = false;  // Disable timer when manually setting state
        gpio_set_level(RELAY_GPIO, state);  // Set the actual GPIO level
    }

    // Set response headers
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Send appropriate response
    if (success) {
        create_json_response(response, sizeof(response),
            "{\"status\":\"ok\",\"state\":\"%s\"}", 
            state ? "on" : "off");  // Use the state we just set
    } else {
        create_json_response(response, sizeof(response),
            "{\"status\":\"error\",\"message\":\"Invalid request\"}");
    }
    
    ret = httpd_resp_sendstr(req, response);
    return (ret == ESP_OK) ? ESP_OK : ESP_FAIL;
}

static esp_err_t timer_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        char response[200];
        create_json_response(response, sizeof(response),
            "{\"enabled\":%s,\"onDuration\":%u,\"offDuration\":%u,\"currentState\":%s}",
            relay_timer.enabled ? "true" : "false",
            relay_timer.on_duration / 1000,
            relay_timer.off_duration / 1000,
            relay_timer.current_state ? "true" : "false");
        
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);
    } else if (req->method == HTTP_POST) {
        char buf[100];
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret <= 0) return ESP_FAIL;
        
        buf[ret] = '\0';
        
        // Simple string parsing
        if (strstr(buf, "\"enabled\":true")) {
            relay_timer.enabled = true;
        } else if (strstr(buf, "\"enabled\":false")) {
            relay_timer.enabled = false;
            gpio_set_level(RELAY_GPIO, 0);
            relay_timer.current_state = false;
        }
        
        char *on_dur = strstr(buf, "\"onDuration\":");
        if (on_dur) {
            relay_timer.on_duration = atoi(on_dur + 13) * 1000;
        }
        
        char *off_dur = strstr(buf, "\"offDuration\":");
        if (off_dur) {
            relay_timer.off_duration = atoi(off_dur + 14) * 1000;
        }

        relay_timer.last_toggle = esp_timer_get_time() / 1000;
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    }
    return ESP_OK;
}

// Add hostname handlers
static esp_err_t hostname_get_handler(httpd_req_t *req) {
    nvs_handle_t nvs_handle;
    char hostname[32] = DEFAULT_HOSTNAME;
    size_t hostname_len = sizeof(hostname);

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        nvs_get_str(nvs_handle, NVS_KEY_HOSTNAME, hostname, &hostname_len);
        nvs_close(nvs_handle);
    }

    char response[64];
    create_json_response(response, sizeof(response), 
        "{\"hostname\":\"%s\"}", hostname);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t hostname_post_handler(httpd_req_t *req) {
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0) return ESP_FAIL;
    
    buf[ret] = '\0';
    
    // Parse hostname from JSON
    char *hostname_start = strstr(buf, "\"hostname\":\"");
    if (hostname_start) {
        hostname_start += 11; // Skip "hostname":"
        char *hostname_end = strchr(hostname_start, '"');
        if (hostname_end) {
            *hostname_end = '\0';
            
            // Store hostname in NVS
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
            if (err == ESP_OK) {
                nvs_set_str(nvs_handle, NVS_KEY_HOSTNAME, hostname_start);
                nvs_commit(nvs_handle);
                nvs_close(nvs_handle);
            }
        }
    }

    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// Timer control task
static void timer_control_task(void *pvParameters) {
    while (1) {
        if (relay_timer.enabled) {
            uint32_t current_time = esp_timer_get_time() / 1000;
            uint32_t elapsed = current_time - relay_timer.last_toggle;
            
            if (relay_timer.current_state && elapsed >= relay_timer.on_duration) {
                gpio_set_level(RELAY_GPIO, 0);
                relay_timer.current_state = false;
                relay_timer.last_toggle = current_time;
            } else if (!relay_timer.current_state && elapsed >= relay_timer.off_duration) {
                gpio_set_level(RELAY_GPIO, 1);
                relay_timer.current_state = true;
                relay_timer.last_toggle = current_time;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Check every 100ms
    }
}
