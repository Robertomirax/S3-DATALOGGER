#---------------- CMakeLists.txt --------------------------
idf_component_register(
    SRCS "hardware.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        freertos
        esp_driver_uart 
        esp_driver_i2c
        esp_driver_gpio 
        esp_driver_spi 
        esp_driver_rmt
        esp_adc
        esp_lcd
        driver
        fatfs
        wear_levelling
        esp_partition
        app_update
        esp_tinyusb
            )