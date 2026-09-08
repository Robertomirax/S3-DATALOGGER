#---------------- CMakeLists.txt --------------------------
idf_component_register(
    SRCS "hardware.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        freertos
        esp_driver_uart 
        esp_driver_gpio 
        esp_driver_spi 
        esp_driver_rmt
        esp_adc
        driver
                )