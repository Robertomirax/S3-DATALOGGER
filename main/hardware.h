//----------------------------   hardware.h ----------------
// Definiciones de pines, buffers y funciones de control del hardware del
// datalogger. Este archivo centraliza la configuración del UART y del LED RGB.

#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>

//#include "driver/gpio.h"
//#include "driver/uart.h"
//#include "driver/spi_master.h"
//#include "esp_lcd_panel_vendor.h"
//#include "esp_lcd_panel_ops.h"
//#include "esp_lvgl_port.h"  // IWYU pragma: keep
//#include "lvgl.h"


#ifdef __cplusplus
extern "C" {
#endif

// --- CONFIGURACIÓN UART / MAX3232 ---
// El puerto UART_1 se usa para la comunicación con la celda de carga.
#define UART_PORT_NUM UART_NUM_1
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_18
#define UART_BUF_SIZE 1024
#define BAUD_RATE 300

// --- OTROS PINES ---
// GPIO del LED de estado y GPIO del LED RGB WS2812.
#define BLINK_GPIO GPIO_NUM_8
#define WS2812_GPIO GPIO_NUM_48

// Pines y resolución previstos para un display ST7789 si se reactivara.
#define LCD_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 240

#define PIN_NUM_SCLK 12
#define PIN_NUM_MOSI 11
#define PIN_NUM_MISO -1
#define PIN_NUM_LCD_DC 2
#define PIN_NUM_LCD_RST 4
#define PIN_NUM_LCD_CS -1
#define PIN_NUM_BK_LIGHT 1

// Inicializa el hardware UART del sistema.
void hardware_init_uart(void);

// Inicializa la capa de almacenamiento y el resto de periféricos conectados.
void hardware_init_all(void);

// Envía un color RGB al LED WS2812 mediante RMT.
void hardware_ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue);


#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H