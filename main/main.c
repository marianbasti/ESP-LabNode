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
#include "mdns.h"

#define TAG "temcontrol"

// GPIO configuration from menuconfig
#define DHT_GPIO CONFIG_GPIO_DHT
#define RELAY_GPIO CONFIG_GPIO_RELAY
#define LED_GPIO CONFIG_GPIO_LED

// WiFi event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Add new constant for default hostname
#define DEFAULT_HOSTNAME "ESP-LabNode"
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

// Add these defines after existing ones
#define AP_SSID "ESP-Config"
#define AP_PASS "configure123"
#define AP_MAX_CONNECTIONS 1
#define WIFI_CONNECT_TIMEOUT_MS 10000

// Add these globals after existing ones
static bool is_ap_mode = false;
static esp_netif_t *ap_netif = NULL;

// Add this HTML string near the top with other defines
static const char *config_html = "<!DOCTYPE html><html>"
    "<head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:20px;} .n{margin:8px 0;}</style></head>"
    "<body><h1>WiFi Configuration</h1>"
    "<div id='networks'></div>"
    "<div class='n'><input type='text' id='ssid' placeholder='SSID'></div>"
    "<div class='n'><input type='password' id='pass' placeholder='Password'></div>"
    "<div class='n'><button onclick='save()'>Save</button> "
    "<button onclick='scan()'>Scan</button></div>"
    "<script>function scan(){fetch('/api/scan').then(r=>r.json()).then(d=>"
    "document.getElementById('networks').innerHTML=d.networks.map(n=>"
    "`<div class='n'><a href='#' onclick='select(\"${n}\")'>${n}</a></div>`).join(''))};"
    "function select(s){document.getElementById('ssid').value=s};"
    "function save(){fetch('/api/wifi',{method:'POST',body:JSON.stringify({ssid:"
    "document.getElementById('ssid').value,pass:document.getElementById('pass').value})})"
    ".then(()=>alert('Saved. Device will restart.'))};scan();</script></body></html>";

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

// Add these new handler declarations after existing ones
static esp_err_t config_get_handler(httpd_req_t *req);
static esp_err_t scan_get_handler(httpd_req_t *req);
static esp_err_t wifi_post_handler(httpd_req_t *req);

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
    .method    = HTTP_GET,
    .handler   = hostname_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t hostname_post_uri = {
    .uri       = "/api/hostname",
    .method    = HTTP_POST,
    .handler   = hostname_post_handler,
    .user_ctx  = NULL
};

// Add these new URI handlers after existing ones
static const httpd_uri_t config_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = config_get_handler,
};

static const httpd_uri_t scan_uri = {
    .uri       = "/api/scan",
    .method    = HTTP_GET,
    .handler   = scan_get_handler,
};

static const httpd_uri_t wifi_uri = {
    .uri       = "/api/wifi",
    .method    = HTTP_POST,
    .handler   = wifi_post_handler,
};

// Start HTTP server
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    if (httpd_start(&server, &config) == ESP_OK) {
        if (is_ap_mode) {
            // AP mode handlers
            httpd_register_uri_handler(server, &config_uri);
            httpd_register_uri_handler(server, &scan_uri);
            httpd_register_uri_handler(server, &wifi_uri);
        } else {
            // Normal mode handlers
            httpd_register_uri_handler(server, &sensor_uri);
            httpd_register_uri_handler(server, &relay_uri);
            httpd_register_uri_handler(server, &timer_uri);        // Register GET handler
            httpd_register_uri_handler(server, &timer_post_uri);   // Register POST handler
            httpd_register_uri_handler(server, &hostname_get_uri);
            httpd_register_uri_handler(server, &hostname_post_uri);
        }
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

// Add these new handler implementations before app_main
static esp_err_t config_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, config_html, strlen(config_html));
    return ESP_OK;
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 500
    };
    
    // Start scan
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));
    
    // Get results
    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count > 20) ap_count = 20;
    
    wifi_ap_record_t ap_records[20];
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));
    
    // Build JSON response
    char *response = malloc(2048);
    strcpy(response, "{\"networks\":[");
    
    for (int i = 0; i < ap_count; i++) {
        if (i > 0) strcat(response, ",");
        strcat(response, "\"");
        strcat(response, (char *)ap_records[i].ssid);
        strcat(response, "\"");
    }
    strcat(response, "]}");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req) {
    char buf[128];
    nvs_handle_t nvs_handle;
    
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    // Parse JSON (simple parsing)
    char ssid[33] = {0};
    char pass[65] = {0};
    char *ssid_start = strstr(buf, "\"ssid\":\"");
    char *pass_start = strstr(buf, "\"pass\":\"");
    
    if (ssid_start && pass_start) {
        ssid_start += 8;
        pass_start += 8;
        char *ssid_end = strchr(ssid_start, '"');
        char *pass_end = strchr(pass_start, '"');
        
        if (ssid_end && pass_end) {
            strncpy(ssid, ssid_start, ssid_end - ssid_start);
            strncpy(pass, pass_start, pass_end - pass_start);
            
            // Store in NVS
            ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
            ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ssid));
            ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_pass", pass));
            ESP_ERROR_CHECK(nvs_commit(nvs_handle));
            nvs_close(nvs_handle);
            
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            
            // Delay restart to allow response to be sent
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            return ESP_OK;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    return ESP_FAIL;
}

// Initialize WiFi
static void initialize_wifi(void) {
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default STA and AP interfaces
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    
    // Try to load saved credentials
    nvs_handle_t nvs_handle;
    char ssid[33] = {0};
    char pass[65] = {0};
    size_t len = sizeof(ssid);
    
    if (nvs_open("storage", NVS_READONLY, &nvs_handle) == ESP_OK) {
        if (nvs_get_str(nvs_handle, "wifi_ssid", ssid, &len) == ESP_OK) {
            len = sizeof(pass);
            if (nvs_get_str(nvs_handle, "wifi_pass", pass, &len) == ESP_OK) {
                // Configure station with saved credentials
                wifi_config_t wifi_config = {
                    .sta = {
                        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                        .pmf_cfg = {
                            .capable = true,
                            .required = false
                        },
                    },
                };
                strcpy((char *)wifi_config.sta.ssid, ssid);
                strcpy((char *)wifi_config.sta.password, pass);
                
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
                ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
                ESP_ERROR_CHECK(esp_wifi_start());
                
                // Wait for connection
                EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                    pdFALSE,
                    pdFALSE,
                    pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
                
                if (bits & WIFI_CONNECTED_BIT) {
                    ESP_LOGI(TAG, "Connected to saved network");
                    nvs_close(nvs_handle);
                    return;
                }
            }
        }
        nvs_close(nvs_handle);
    }
    
    // If we get here, either no saved credentials or connection failed
    ESP_LOGI(TAG, "Starting AP mode");
    is_ap_mode = true;
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .max_connection = AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Start configuration web server
    start_webserver();
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
        hostname_start += 11;
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
                
                // Update network hostname
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif) {
                    esp_netif_set_hostname(netif, hostname_start);
                }
                
                // Update mDNS hostname
                mdns_hostname_set(hostname_start);
                mdns_instance_name_set(hostname_start);
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
