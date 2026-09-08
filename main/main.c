//-------------------------------- main.c --------------------------------

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware.h"
#include <errno.h> // IWYU pragma: keep
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Constantes y Definiciones
// ---------------------------------------------------------------------------
static const char *TAG = "MAIN_APP";
static const char *TAG_FLASH = "FLASH_WRITER";
static const char *log_path = "/archivos/log_uart.txt";

// static const char DL_HEADER1[] = "\x55\x55\x55\x55\x55\x55\x55";
static const char DL_HEADER2[] = "\r\n\r\nAlert Technologies\r\nDATALOGGER "
                                 "VER1.04\r\n\nMemory Used...08%\r\n";

#define RING_BUF_SIZE (8 * 1024)   // Búfer circular de 8 KB en RAM
#define FLASH_WRITE_THRESHOLD 2048 // Escribir a Flash al acumular 2 KB
#define TEMP_WRITE_BUF_SIZE 2048

// ---------------------------------------------------------------------------
// Estructura del Búfer Circular (Ring Buffer Thread-Safe)
// ---------------------------------------------------------------------------
typedef struct {
  uint8_t buffer[RING_BUF_SIZE];
  size_t head;
  size_t tail;
  size_t count;
  SemaphoreHandle_t mutex;
} ring_buffer_t;

static ring_buffer_t rb;

// Verifica que el mutex del buffer circular haya sido inicializado.
static bool ring_buffer_ready(void) { return rb.mutex != NULL; }

// Inicializa el buffer circular y crea el mutex de protección.
static void ring_buffer_init(void) {
  rb.head = 0;
  rb.tail = 0;
  rb.count = 0;
  rb.mutex = xSemaphoreCreateMutex();
  if (rb.mutex == NULL) {
    ESP_LOGE(TAG, "No se pudo crear el mutex del ring buffer.");
  }
}

// Escribe bytes al buffer circular protegidos por mutex.
static size_t ring_buffer_write(const uint8_t *data, size_t len) {
  if (!ring_buffer_ready() || data == NULL || len == 0)
    return 0;

  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) != pdTRUE)
    return 0;

  size_t bytes_written = 0;
  for (size_t i = 0; i < len; i++) {
    if (rb.count < RING_BUF_SIZE) {
      rb.buffer[rb.head] = data[i];
      rb.head = (rb.head + 1) % RING_BUF_SIZE;
      rb.count++;
      bytes_written++;
    } else {
      ESP_LOGW(TAG, "Ring Buffer Lleno! Se descartaron bytes.");
      break;
    }
  }

  xSemaphoreGive(rb.mutex);
  return bytes_written;
}

// Intenta guardar un bloque completo en RAM, reintentando si el buffer está ocupado.
static bool ring_buffer_write_all(const uint8_t *data, size_t len) {
  if (!ring_buffer_ready() || data == NULL || len == 0)
    return false;

  size_t offset = 0;
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

  while (offset < len) {
    size_t written = ring_buffer_write(data + offset, len - offset);
    offset += written;
    if (offset == len)
      return true;
    if (xTaskGetTickCount() >= deadline)
      break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  ESP_LOGE(TAG, "No se pudo guardar el bloque completo en RAM: %u/%u bytes", (unsigned int)offset, (unsigned int)len);
  return false;
}

// Copia datos del buffer circular hacia un buffer externo sin consumirlos.
static size_t ring_buffer_peek(uint8_t *out_buf, size_t max_len) {
  if (!ring_buffer_ready() || out_buf == NULL || max_len == 0)
    return 0;

  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) != pdTRUE)
    return 0;

  size_t bytes_read = 0;
  size_t index = rb.tail;
  while (bytes_read < max_len && bytes_read < rb.count) {
    out_buf[bytes_read++] = rb.buffer[index];
    index = (index + 1) % RING_BUF_SIZE;
  }

  xSemaphoreGive(rb.mutex);
  return bytes_read;
}

// Elimina bytes ya procesados del buffer circular y avanza la cola.
static size_t ring_buffer_drop(size_t len) {
  if (!ring_buffer_ready())
    return 0;

  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) != pdTRUE)
    return 0;

  size_t bytes_dropped = len < rb.count ? len : rb.count;
  rb.tail = (rb.tail + bytes_dropped) % RING_BUF_SIZE;
  rb.count -= bytes_dropped;

  xSemaphoreGive(rb.mutex);
  return bytes_dropped;
}

// Vacía el contenido del buffer circular para reiniciar el almacenamiento en RAM.
static void ring_buffer_clear(void) {
  if (!ring_buffer_ready())
    return;

  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) == pdTRUE) {
    rb.head = 0;
    rb.tail = 0;
    rb.count = 0;
    xSemaphoreGive(rb.mutex);
  }
}

// Devuelve cuántos bytes hay pendientes actualmente en el buffer circular.
static size_t ring_buffer_get_count(void) {
  size_t count = 0;
  if (!ring_buffer_ready())
    return count;

  if (xSemaphoreTake(rb.mutex, portMAX_DELAY) == pdTRUE) {
    count = rb.count;
    xSemaphoreGive(rb.mutex);
  }
  return count;
}

// ---------------------------------------------------------------------------
// Variables Globales de Estado y UI
// ---------------------------------------------------------------------------
// Estas variables coordinan el flujo principal del sistema: comandos RS-232,
// transmisión del log, activación de secuencia y temporización del LED.

static volatile bool esperando_confirmacion = false;
static volatile bool transmitiendo_archivo = false;
static volatile bool secuencia_activa = false;
static volatile TickType_t ultima_recepcion_secuencia = 0;
static volatile TickType_t inicio_parpadeo_secuencia = 0;

// Mutex para evitar colisiones de lectura/escritura/borrado en el archivo
static SemaphoreHandle_t file_mutex = NULL;

// La máquina de estados interpreta cada trama recibida para separar campos,
// ignorar ceros y Tare, y dejar la línea lista para almacenamiento.
typedef enum {
  WAIT_CRLF_1,
  WAIT_COMMA_1,
  TRIM_ZEROS_2,
  CAPTURE_TO_COMMA_2,
  IGNORE_TO_COMMA_3,
  TRIM_ZEROS_4,
  CAPTURE_TO_COMMA_4,
  WAIT_TARE
} subestado_loop_t;

static subestado_loop_t subestado_loop = WAIT_CRLF_1;
static uint8_t last_byte = 0x00;
static int linea = 0x00;

// Buffer de Registro de Línea en RAM
static char line_buf[128];
static size_t line_idx = 0;
static size_t pos_flag_tara = 0;
static bool line_overflow = false;

// Reinicia el buffer de línea para comenzar una nueva trama desde cero.
static void reset_line_buffer(void) {
  line_idx = 0;
  pos_flag_tara = 0;
  line_overflow = false;
  memset(line_buf, 0, sizeof(line_buf));
}

// Restablece el estado completo del parser para una nueva captura de datos.
static void reset_capture_state(void) {
  subestado_loop = WAIT_CRLF_1;
  last_byte = 0x00;
  linea = 0;
  reset_line_buffer();
}

// Agrega un único carácter al buffer de línea si hay espacio disponible.
static void append_char_to_line(char c) {
  if (line_idx < sizeof(line_buf) - 1) {
    line_buf[line_idx++] = c;
    line_buf[line_idx] = '\0';
  } else {
    line_overflow = true;
  }
}

// Agrega una cadena al buffer de línea, marcando overflow si supera el tamaño.
static void append_str_to_line(const char *str) {
  while (*str && (line_idx < sizeof(line_buf) - 1)) {
    line_buf[line_idx++] = *str++;
  }
  line_buf[line_idx] = '\0';
  if (*str != '\0')
    line_overflow = true;
}

// Detecta si la secuencia UART incluye la cabecera de identificación del equipo.
static bool alerta_tecnologias_recibida(const char *stream) {
  if (stream == NULL)
    return false;

  const char *header = "Alert Technologies, Model ";
  const char *model_start = strstr(stream, header);
  if (model_start == NULL)
    return false;

  // Busca ", Version " a partir de donde termina el encabezado del modelo
  const char *version_start = strstr(model_start + strlen(header), ", Version ");

  return version_start != NULL;
}

// ---------------------------------------------------------------------------
// Procesador byte a byte
// ---------------------------------------------------------------------------
// Parsea la secuencia UART y reconstruye cada línea de datos antes de guardarla.
static void procesar_loop_secuencia(uint8_t *buf, size_t len) {
  if (buf == NULL || len == 0) {
    return;
  }

  TickType_t ahora = xTaskGetTickCount();

  if (!secuencia_activa) {
    secuencia_activa = true;
    inicio_parpadeo_secuencia = ahora;
    hardware_ws2812_set_color(0, 255, 0);
    ESP_LOGI(TAG, "Primera entrada en la secuencia. LED verde encendido.");
  }
  ultima_recepcion_secuencia = ahora;

  for (size_t i = 0; i < len; i++) {
    uint8_t b = buf[i];

    switch (subestado_loop) {
    case WAIT_CRLF_1:
      if (last_byte == 0x0D && b == 0x0A) {
        ESP_LOGI(TAG, "CRLF detectado. Parseando trama en RAM...");
        reset_line_buffer();
        append_str_to_line("\r\n");
        pos_flag_tara = line_idx;
        append_str_to_line("0 ");
        subestado_loop = WAIT_COMMA_1;
      }
      break;

    case WAIT_COMMA_1:
      if (b == ',')
        subestado_loop = TRIM_ZEROS_2;
      break;

    case TRIM_ZEROS_2:
      if (b == ',') {
        append_char_to_line(' ');
        subestado_loop = IGNORE_TO_COMMA_3;
      } else if (b != '0' && b != ' ') {
        append_char_to_line((char)b);
        subestado_loop = CAPTURE_TO_COMMA_2;
      }
      break;

    case CAPTURE_TO_COMMA_2:
      if (b == ',') {
        append_char_to_line(' ');
        subestado_loop = IGNORE_TO_COMMA_3;
      } else {
        append_char_to_line((char)b);
      }
      break;

    case IGNORE_TO_COMMA_3:
      if (b == ',')
        subestado_loop = TRIM_ZEROS_4;
      break;

    case TRIM_ZEROS_4:
      if (b == ',') {
        subestado_loop = WAIT_TARE;
      } else if (b != '0' && b != ' ') {
        append_char_to_line((char)b);
        subestado_loop = CAPTURE_TO_COMMA_4;
      }
      break;

    case CAPTURE_TO_COMMA_4:
      if (b == ',') {
        subestado_loop = WAIT_TARE;
      } else {
        append_char_to_line((char)b);
      }
      break;

    case WAIT_TARE:
      if (b != '0' && b != '1')
        break;

      ESP_LOGI(TAG, "Byte Tare: 0x%02X", b);
      if (line_overflow) {
        ESP_LOGW(TAG, "Linea descartada: excede %u bytes", (unsigned int)(sizeof(line_buf) - 1));
        subestado_loop = WAIT_CRLF_1;
        break;
      }
      if (b == '1' && pos_flag_tara < line_idx) {
        line_buf[pos_flag_tara] = '1';
      }

      // Depositar línea procesada en el Ring Buffer de RAM
      if (line_idx > 0) {
        if (ring_buffer_write_all((uint8_t *)line_buf, line_idx)) {
          ESP_LOGI(TAG, "Añadido a RAM Ring Buffer: %s", line_buf);
        } else {
          ESP_LOGW(TAG, "Línea descartada por falta de espacio en RAM: %s", line_buf);
        }
      }

      subestado_loop = WAIT_CRLF_1;
      linea++;

      if ((linea) % 256 == 0) {
        char seq_buf[32];
        int seq_len = snprintf(seq_buf, sizeof(seq_buf), "\r\nSeq#..%05d", linea);
        if (!ring_buffer_write_all((uint8_t *)seq_buf, seq_len))
          ESP_LOGE(TAG, "Secuencia de control descartada parcialmente.");
        ESP_LOGI(TAG, "Añadida línea de control a RAM: %s", seq_buf);
      }
      break;
    }
    last_byte = b;
  }
}

// ---------------------------------------------------------------------------
// Tareas Secundarias (Flash Writer)
// ---------------------------------------------------------------------------
// Esta tarea asegura que el contenido acumulado en RAM se vuelque a la
// partición LittleFS periódicamente o cuando supera el umbral definido.
static void flash_writer_task(void *arg) {
  uint8_t *write_buf = (uint8_t *)malloc(TEMP_WRITE_BUF_SIZE);
  if (!write_buf) {
    ESP_LOGE(TAG_FLASH, "Fallo al asignar memoria para write_buf");
    vTaskDelete(NULL);
    return;
  }

  TickType_t last_flush_time = xTaskGetTickCount();

  while (1) {
    size_t pending_bytes = ring_buffer_get_count();
    TickType_t elapsed = xTaskGetTickCount() - last_flush_time;

    // Criterio de volcado: 2 KB acumulados O transcurridos 5 segundos
    bool should_write = (pending_bytes >= FLASH_WRITE_THRESHOLD) || (pending_bytes > 0 && elapsed >= pdMS_TO_TICKS(5000));

    if (should_write && !transmitiendo_archivo) {
      if (file_mutex == NULL) {
        ESP_LOGE(TAG_FLASH, "Mutex de archivo no inicializado.");
      } else if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        FILE *f = fopen(log_path, "a+");
        if (f != NULL) {
          while (ring_buffer_get_count() > 0) {
            size_t bytes_to_read = ring_buffer_peek(write_buf, TEMP_WRITE_BUF_SIZE);
            if (bytes_to_read == 0) {
              break;
            }

            size_t bytes_written = fwrite(write_buf, 1, bytes_to_read, f);
            if (bytes_written != bytes_to_read) {
              ESP_LOGE(TAG_FLASH, "Error escribiendo log: %u/%u bytes", (unsigned int)bytes_written, (unsigned int)bytes_to_read);
              if (ferror(f)) {
                ESP_LOGE(TAG_FLASH, "Error del flujo de archivo: %d", errno);
              }
              break;
            }

            ESP_LOGI(TAG_FLASH, "Grabado en flash (%u bytes): %.*s", (unsigned int)bytes_written, (int)bytes_written,
                     (const char *)write_buf);
            ring_buffer_drop(bytes_written);
          }

          if (fflush(f) != 0 || ferror(f)) {
            ESP_LOGE(TAG_FLASH, "Error confirmando escritura del log.");
          }
          fclose(f);
          ESP_LOGI(TAG_FLASH, "Volcado de RAM a Flash realizado correctamente.");
        } else {
          ESP_LOGE(TAG_FLASH, "Error abriendo archivo log.");
        }
        xSemaphoreGive(file_mutex);
      }
      last_flush_time = xTaskGetTickCount();
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }

  free(write_buf);
  vTaskDelete(NULL);
}

// Controla la señal visual del LED según la actividad y la pérdida de datos UART.
static void secuencia_led_task(void *arg) {
  int estado_led = -1;

  while (1) {
    if (secuencia_activa) {
      TickType_t ahora = xTaskGetTickCount();
      TickType_t silencio = ahora - ultima_recepcion_secuencia;
      bool error = silencio > pdMS_TO_TICKS(30000);
      TickType_t periodo = error ? pdMS_TO_TICKS(250) : pdMS_TO_TICKS(4000);
      TickType_t fase = error ? ahora : ahora - inicio_parpadeo_secuencia;
      bool encendido = (fase % periodo) < (error ? pdMS_TO_TICKS(125) : pdMS_TO_TICKS(2000));
      int nuevo_estado = error ? (encendido ? 2 : 0) : (encendido ? 1 : 0);

      if (nuevo_estado != estado_led) {
        if (nuevo_estado == 2) {
          hardware_ws2812_set_color(LED_COLOR_MAGENTA);
        } else if (nuevo_estado == 1) {
          hardware_ws2812_set_color(LED_COLOR_VERDE);
        } else {
          hardware_ws2812_set_color(0, 0, 0);
        }
        estado_led = nuevo_estado;

        if (error && nuevo_estado == 2)
          ESP_LOGE(TAG, "Sin datos UART durante mas de 30 segundos. LED rojo parpadeando.");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------------------------------------------------------------------
// Tareas Principales
// ---------------------------------------------------------------------------

// Mide la batería y deja el valor en la terminal de depuración cada 10 segundos.
static void battery_voltage_task(void *arg) {
  while (1) {
    int battery_voltage_mv = hardware_read_battery_voltage_mv();
    if (battery_voltage_mv >= 0) {
      ESP_LOGI(TAG, "Voltaje de bateria: %d mV (%.2f V)", battery_voltage_mv, battery_voltage_mv / 1000.0f);
      if (battery_voltage_mv < BATTERY_LOW_AMARILLO_MV && battery_voltage_mv >= BATTERY_LOW_ROJO_MV) {
        hardware_ws2812_flash_color(255, 180, 0, 100);
        ESP_LOGW(TAG, "Bateria baja: destello amarillo");
      }else if (battery_voltage_mv < BATTERY_LOW_ROJO_MV) {
        hardware_ws2812_flash_color(255, 0, 0, 100);
        ESP_LOGW(TAG, "Bateria muy baja: destello rojo");
      }
    } else {
      ESP_LOGE(TAG, "No se pudo leer el voltaje de bateria");
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// Transmite el archivo de log almacenado en flash a través del puerto UART.
static void tx_file_task(void *arg) {
  uint8_t *tx_buffer = (uint8_t *)malloc(UART_BUF_SIZE);
  if (tx_buffer == NULL) {
    ESP_LOGE(TAG, "No se pudo reservar el buffer de transmision.");
    transmitiendo_archivo = false;
    vTaskDelete(NULL);
    return;
  }

  vTaskDelay(pdMS_TO_TICKS(10000));

  if (uart_set_baudrate(UART_PORT_NUM, 4800) == ESP_OK) {
    ESP_LOGI(TAG, "Baudrate cambiado a 4800");
  }

  if (file_mutex == NULL) {
    ESP_LOGE(TAG, "Mutex de archivo no inicializado para transmision.");
  } else if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    FILE *f = fopen(log_path, "a+");
    if (f != NULL) {
      while (ring_buffer_get_count() > 0) {
        size_t bytes_to_write = ring_buffer_peek(tx_buffer, UART_BUF_SIZE);
        if (bytes_to_write == 0)
          break;

        size_t bytes_written = fwrite(tx_buffer, 1, bytes_to_write, f);
        if (bytes_written != bytes_to_write) {
          ESP_LOGE(TAG, "Error guardando datos pendientes antes de transmitir.");
          if (ferror(f)) {
            ESP_LOGE(TAG, "Error del flujo de archivo: %d", errno);
          }
          break;
        }
        ring_buffer_drop(bytes_written);
      }
      fflush(f);
      fclose(f);

      f = fopen(log_path, "r");
    }
    if (f != NULL) {
      size_t bytes_read = 0;
      while ((bytes_read = fread(tx_buffer, 1, UART_BUF_SIZE, f)) > 0) {
        uart_write_bytes(UART_PORT_NUM, (const char *)tx_buffer, bytes_read);
        uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(1000));
      }
      if (ferror(f))
        ESP_LOGE(TAG, "Error leyendo archivo log para transmision.");
      fclose(f);
    } else {
      ESP_LOGE(TAG, "Error abriendo archivo log para lectura");
    }
    xSemaphoreGive(file_mutex);
  }

  const char *msg = "\r\n\r\nChangeBaud->300\r\n\r\n";
  uart_write_bytes(UART_PORT_NUM, msg, strlen(msg));
  if (uart_wait_tx_done(UART_PORT_NUM, pdMS_TO_TICKS(1500)) != ESP_OK)
    ESP_LOGW(TAG, "Timeout esperando el mensaje de cambio de baudrate.");

  uart_set_baudrate(UART_PORT_NUM, 300);

  free(tx_buffer);
  transmitiendo_archivo = false;
  vTaskDelete(NULL);
}

// Recibe datos UART y coordina el parseo, comandos RS-232 y captura de tramas.
static void rx_task(void *arg) {
  uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
  char *stream_buf = (char *)malloc(UART_BUF_SIZE + 64);

  if (rb.mutex == NULL) {
    ESP_LOGE(TAG, "Ring buffer no inicializado. Abortando rx_task.");
    free(data);
    free(stream_buf);
    vTaskDelete(NULL);
    return;
  }

  if (data == NULL || stream_buf == NULL) {
    ESP_LOGE(TAG, "Error asignando memoria estática para rx_task");
    if (data)
      free(data);
    if (stream_buf)
      free(stream_buf);
    vTaskDelete(NULL);
    return;
  }

  uint8_t estado_grabado = 0;
  const char *target = "seq #\",\"ld cella\",\"     dac\",\"    temp\",\"    tare\"";
  size_t target_len = strlen(target);
  bool alerta_led_azul = false;

  char overlap_buf[64] = {0};
  size_t overlap_len = 0;

  while (1) {
    int rxBytes = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));

    if (rxBytes > 0) {
      ESP_LOGD(TAG, "UART RX: %d bytes", rxBytes);

      // flag que evita que se guarde o procese el contenido si se está atendiendo
      // un comando del usuario o una transmisión de datos desde la memoria.
      bool ignorar_grabacion = false;
      size_t total_stream_len = overlap_len + rxBytes;

      memcpy(stream_buf, overlap_buf, overlap_len);
      memcpy(stream_buf + overlap_len, data, rxBytes);
      stream_buf[total_stream_len] = '\0';

      // Verificar si se ha recibido la cabecera de "Alert Technologies"
      // Si se detecta, encender el LED azul y registrar el evento
      if (!alerta_led_azul && alerta_tecnologias_recibida(stream_buf)) {
        hardware_ws2812_set_color(0, 0, 255);
        alerta_led_azul = true;
        ESP_LOGI(TAG, "Alerta de tecnologias detectada. LED azul encendido.");
      }

      // 1. Manejo de Comandos RS-232
      // Comprueba si el sistema está esperando una respuesta de confirmación
      // para borrar RAM o ejecutar una operación controlada.
      if (esperando_confirmacion) {
        char resp = 0;
        for (int i = 0; i < rxBytes; i++) {
          if (data[i] == 'y' || data[i] == 'Y' || data[i] == 'n' || data[i] == 'N') {
            resp = (char)data[i];
            break;
          }
        }
        if (resp == 'y' || resp == 'Y') {
          uart_write_bytes(UART_PORT_NUM, "Self Diag ...Waiting\r\n", 22);

          if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            FILE *f_clr = fopen(log_path, "w");
            if (f_clr != NULL) {
              fclose(f_clr);
              ring_buffer_clear();
              reset_capture_state();
              uart_write_bytes(UART_PORT_NUM, "RAM test successful\r\n\r\nDone\r\n", 29);
            }
            xSemaphoreGive(file_mutex);
          }
        }
        esperando_confirmacion = false;
        ignorar_grabacion = true;
        overlap_len = 0;
        memset(overlap_buf, 0, sizeof(overlap_buf));

      } else if (strstr(stream_buf, "send") != NULL) {
        if (!transmitiendo_archivo) {
          transmitiendo_archivo = true;
          const char *cambio = "ChangeBaud->4800 in 10Sec\r\n";
          uart_write_bytes(UART_PORT_NUM, cambio, strlen(cambio));
          if (xTaskCreate(tx_file_task, "tx_file_task", 4096, NULL, 5, NULL) != pdPASS) {
            transmitiendo_archivo = false;
            ESP_LOGE(TAG, "No se pudo crear la tarea de transmision.");
          }
        }
        ignorar_grabacion = true;
        overlap_len = 0;
        memset(overlap_buf, 0, sizeof(overlap_buf));

      } else if (strstr(stream_buf, "mtest") != NULL) {
        esperando_confirmacion = true;
        const char *prompt = "Will clear data\r\nAre you sure? y/n  \r\n";
        uart_write_bytes(UART_PORT_NUM, prompt, strlen(prompt));
        ignorar_grabacion = true;
        overlap_len = 0;
        memset(overlap_buf, 0, sizeof(overlap_buf));
      }

      // 2. Procesamiento de Cabecera y Trama de Datos
      // Busca la cabecera del protocolo y, si ya está activa la captura,
      // parsea cada tramo de datos para reconstruir líneas útiles.
      if (!ignorar_grabacion && !transmitiendo_archivo) {
        if (estado_grabado == 0) {
          char *match = (char *)memmem(stream_buf, total_stream_len, target, target_len);

          if (match != NULL) {
            size_t stream_match_idx = match - stream_buf;
            size_t data_match_end_idx =
                (stream_match_idx >= overlap_len) ? (stream_match_idx - overlap_len + target_len) : (target_len - (overlap_len - stream_match_idx));

            // Guardar la cabecera en la RAM
            if (!ring_buffer_write_all(data, data_match_end_idx))
              ESP_LOGE(TAG, "Cabecera descartada parcialmente por falta de espacio en RAM.");

            // Transición a la captura continua de tramas

            ESP_LOGI(TAG, "Cabecera detectada. Iniciando captura de tramas...");
            estado_grabado = 2;
            reset_capture_state();

            // Procesar el resto del buffer inmediatamente en la máquina de estados
            if (rxBytes > data_match_end_idx) {
              procesar_loop_secuencia(data + data_match_end_idx, rxBytes - data_match_end_idx);
            }
          } else {
            // Guardar texto inicial antes de la cabecera
            if (!ring_buffer_write_all(data, rxBytes))
              ESP_LOGE(TAG, "Datos UART descartados parcialmente por falta de espacio en RAM.");
          }
        } else if (estado_grabado == 2) {
          // Captura continua activa: procesar todos los bytes entrantes
          procesar_loop_secuencia(data, rxBytes);
        }
      }

      // 3. Mantenimiento del buffer de solapamiento
      // Conserva un tramo final del flujo para detectar coincidencias partidas
      // entre bloques UART consecutivos.
      if (!ignorar_grabacion && !transmitiendo_archivo) {
        if (total_stream_len >= (target_len - 1)) {
          overlap_len = target_len - 1;
          if (overlap_len > sizeof(overlap_buf)) {
            overlap_len = sizeof(overlap_buf);
          }
          memcpy(overlap_buf, stream_buf + total_stream_len - overlap_len, overlap_len);
        } else {
          overlap_len = total_stream_len;
          if (overlap_len > sizeof(overlap_buf)) {
            overlap_len = sizeof(overlap_buf);
          }
          memcpy(overlap_buf, stream_buf, overlap_len);
        }
      }
    }
  }

  free(data);
  free(stream_buf);
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Entrada Principal (Main)
// ---------------------------------------------------------------------------
// Punto de entrada del firmware: crea los mutex, inicializa el hardware,
// restaura el estado de captura y lanza las tareas del sistema.
void app_main(void) {
  ESP_LOGI(TAG, "Inicializando Hardware y Estructuras de Memoria...");

  file_mutex = xSemaphoreCreateMutex();
  if (file_mutex == NULL) {
    ESP_LOGE(TAG, "No se pudo crear el mutex de archivos.");
    return;
  }
  ring_buffer_init();
  if (rb.mutex == NULL) {
    ESP_LOGE(TAG, "No se pudo crear el mutex del ring buffer.");
    return;
  }

  hardware_init_all();
  reset_capture_state();
  ESP_LOGI(TAG, "Creando Interfaz de Usuario...");

  // Creación de tareas FreeRTOS
  if (xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, 5, NULL) != pdPASS ||
      xTaskCreate(flash_writer_task, "flash_writer_task", 4096, NULL, 4, NULL) != pdPASS ||
      xTaskCreate(secuencia_led_task, "secuencia_led_task", 2048, NULL, 3, NULL) != pdPASS ||
      xTaskCreate(battery_voltage_task, "battery_voltage_task", 2048, NULL, 3, NULL) != pdPASS) {
    ESP_LOGE(TAG, "No se pudieron crear todas las tareas del sistema.");
    return;
  }

  uart_write_bytes(UART_PORT_NUM, DL_HEADER2, strlen(DL_HEADER2));

  vTaskDelete(NULL);
}