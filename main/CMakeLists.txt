idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif nvs_flash esp_timer lwip esp_driver_gpio esp_driver_spi)
