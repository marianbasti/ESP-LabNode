idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES mdns esp_wifi nvs_flash esp_http_server esp_netif esp32-dht driver)
