//----------------------------   hardware.h ----------------
// Definiciones de pines, buffers y funciones de control del hardware del
// datalogger. Este archivo centraliza la configuración del UART y del LED RGB.

#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>

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
// GPIO del LED RGB WS2812.
#define WS2812_GPIO GPIO_NUM_48
#define BAT_VOLTAGE_ADC_PIN GPIO_NUM_5 // ADC1_CH4
#define BAT_VOLTAGE_DIVIDER_RATIO 2.03f // Ajustar al divisor resistivo usado
#define BATTERY_LOW_AMARILLO_MV 4000 // Umbral de batería baja nivel amarillo en mV
#define BATTERY_LOW_ROJO_MV 3800 // Umbral de batería baja nivel rojo en mV
#define LED_COLOR_MAGENTA 255, 0, 255 // Color magenta para indicar inicio esperando primera trama de la celda de carga
#define LED_COLOR_VERDE 0, 255, 0 // Color verde para indicar recepción de datos
#define LED_COLOR_ROJO 255, 0, 0 // Color rojo para indicar error o falta de datos

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

// Inicializa el ADC usado para medir la batería.
void hardware_init_battery_adc(void);

// Lee la tensión de batería en milivoltios.
int hardware_read_battery_voltage_mv(void);

// Inicializa la capa de almacenamiento y el resto de periféricos conectados.
void hardware_init_all(void);

// Envía un color RGB al LED WS2812 mediante RMT.
void hardware_ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue);

// Muestra un color brevemente y restaura el color anterior.
void hardware_ws2812_flash_color(uint8_t red, uint8_t green, uint8_t blue, uint32_t duration_ms);


#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H