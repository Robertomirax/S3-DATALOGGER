//----------------------------   hardware.h ----------------
// Definiciones de pines, buffers y funciones de control del hardware del
// datalogger. Este archivo centraliza la configuración del UART y del OLED.

#ifndef HARDWARE_H
#define HARDWARE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

#define FIRM 37 // Version del firmware

// --- CONFIGURACIÓN UART / MAX3232 ---
// El puerto UART_1 se usa para la comunicación con la celda de carga.
#define UART_PORT_NUM UART_NUM_1
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_18
#define UART_BUF_SIZE 1024
#define BAUD_RATE 300

// --- OTROS PINES ---
#define BAT_VOLTAGE_ADC_PIN GPIO_NUM_5  // ADC1_CH4
#define BAT_VOLTAGE_DIVIDER_RATIO 2.03f // Ajustar al divisor resistivo usado
#define BATTERY_LOW_AMARILLO_MV 4000    // Umbral de batería baja nivel amarillo en mV
#define BATTERY_LOW_ROJO_MV 3800        // Umbral de batería baja nivel rojo en mV
#define OLED_I2C_SDA_GPIO GPIO_NUM_11
#define OLED_I2C_SCL_GPIO GPIO_NUM_12
#define OLED_I2C_ADDRESS 0x3C

// Inicializa el hardware UART del sistema.
void hardware_init_uart(void);

// Inicializa el ADC usado para medir la batería.
void hardware_init_battery_adc(void);

// Lee la tensión de batería en milivoltios.
int hardware_read_battery_voltage_mv(void);

// Inicializa el OLED SSD1306 conectado por I2C.
void hardware_init_oled(void);

// Actualiza el voltaje de batería mostrado en la primera línea del OLED.
void hardware_oled_show_battery(int voltage_mv);

// Oculta la línea de batería del OLED cuando el equipo está en modo USB.
void hardware_oled_hide_battery(void);

// Muestra un estado o aviso en el OLED.
void hardware_oled_show_message(const char *title, const char *message);

// Actualiza el porcentaje de tara mostrado en la segunda línea.
void hardware_oled_show_tara(float tara_percent);

// Muestra el contenido de la última trama recibida debajo de la batería.
void hardware_oled_show_frame(const char *frame);

// Muestra los últimos caracteres recibidos por UART en la última línea.
void hardware_oled_update_uart_preview(const uint8_t *data, size_t len);

// Oculta la vista previa UART cuando comienza la captura de tramas.
void hardware_oled_clear_uart_preview(void);

// Muestra en el OLED los datos originales recibidos durante el loop.
void hardware_oled_update_loop_preview(const uint8_t *data, size_t len);

// Inicializa la capa de almacenamiento y el resto de periféricos conectados.
void hardware_init_all(void);

// Indica si el almacenamiento está reservado para el host USB como pendrive.
bool hardware_usb_msc_active(void);

// Indica que el host USB se desconecto fisicamente del pendrive.
bool hardware_usb_msc_detached(void);

// Cede la particion FAT al host USB sin reiniciar ni reflashear el equipo.
esp_err_t hardware_enter_usb_msc(void);

// Detiene MSC y devuelve la particion FAT a la aplicacion.
esp_err_t hardware_exit_usb_msc(void);

#ifdef __cplusplus
}
#endif

#endif // HARDWARE_H