//-------------------------------- main.c --------------------------------

#include "driver/uart.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware.h"
#include <errno.h> // IWYU pragma: keep
#include <stdbool.h>
#include <ctype.h>
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
static const char *update_path = "/archivos/S3-DATALOGGER.bin";

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
// transmisión del log y activación de secuencia.

static volatile bool esperando_confirmacion = false;
static volatile bool transmitiendo_archivo = false;
static volatile bool secuencia_activa = false;
static volatile bool cabecera_recibida = false;
static volatile bool modo_usb = false;
static volatile TickType_t ultima_recepcion_secuencia = 0;

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

// Estado del detector de "Running:" y del último valor numérico recibido.
static char ultimo_valor_numerico[32];
static size_t ultimo_valor_len = 0;
static char valor_numerico_actual[32];
static size_t valor_numerico_len = 0;
static const char running_header[] =
  "\"Running:\"\r\n\"   seq #\",\"ld cella\",\"     dac\",\"    temp\",\"    tare\"";
static char running_context[sizeof(running_header)];
static size_t running_context_len = 0;

static void guardar_valor_numerico_actual(void) {
  if (valor_numerico_len == 0)
    return;

  memcpy(ultimo_valor_numerico, valor_numerico_actual, valor_numerico_len);
  ultimo_valor_numerico[valor_numerico_len] = '\0';
  ultimo_valor_len = valor_numerico_len;
  valor_numerico_len = 0;
}

// Busca el encabezado completo y muestra el último número, incluso si llegan en bloques distintos.
static void procesar_running(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];

    if (isdigit((unsigned char)c) ||
        ((c == '-' || c == '+' || c == '.') && valor_numerico_len == 0)) {
      if (valor_numerico_len < sizeof(valor_numerico_actual) - 1)
        valor_numerico_actual[valor_numerico_len++] = c;
    } else {
      guardar_valor_numerico_actual();
    }

    if (running_context_len == sizeof(running_context) - 1) {
      memmove(running_context, running_context + 1, running_context_len - 1);
      running_context_len--;
    }
    running_context[running_context_len++] = c;
    running_context[running_context_len] = '\0';

    if (strstr(running_context, running_header) != NULL) {
      if (ultimo_valor_len > 0) {
        ESP_LOGI(TAG, "Running: ultimo valor numerico: %s", ultimo_valor_numerico);
        float valor_numerico = strtof(ultimo_valor_numerico, NULL);
        float tara = 100.0f - (valor_numerico * 100.0f / 4096.0f);
        hardware_oled_show_tara(tara);
        ESP_LOGI(TAG, "Tara calculada: %.2f %%", tara);
      } else {
        ESP_LOGW(TAG, "Running: no se recibio un valor numerico previo");
      }
      running_context_len = 0;
    }
  }
}

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
    ESP_LOGI(TAG, "Primera entrada en la secuencia.");
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

      hardware_oled_show_frame(line_buf);

      if (!ring_buffer_write_all((const uint8_t *)line_buf, line_idx)) {
        ESP_LOGE(TAG, "No se pudo guardar la trama compactada en RAM.");
      }

      subestado_loop = WAIT_CRLF_1;
      linea++;

      if ((linea) % 256 == 0) {
        char seq_buf[32];
        snprintf(seq_buf, sizeof(seq_buf), "\r\nSeq#..%05d", linea);
        ESP_LOGI(TAG, "Secuencia procesada: %s", seq_buf);
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

  while (!modo_usb) {
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

static bool flush_pending_log(void) {
  uint8_t *write_buf = (uint8_t *)malloc(TEMP_WRITE_BUF_SIZE);
  if (write_buf == NULL || file_mutex == NULL) {
    free(write_buf);
    return false;
  }

  bool success = false;
  if (xSemaphoreTake(file_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    FILE *file = fopen(log_path, "a+");
    if (file != NULL) {
      while (ring_buffer_get_count() > 0) {
        size_t bytes_to_read = ring_buffer_peek(write_buf, TEMP_WRITE_BUF_SIZE);
        if (bytes_to_read == 0 || fwrite(write_buf, 1, bytes_to_read, file) != bytes_to_read) {
          break;
        }
        ring_buffer_drop(bytes_to_read);
      }
      success = fflush(file) == 0 && ring_buffer_get_count() == 0;
      fclose(file);
    }
    xSemaphoreGive(file_mutex);
  }

  free(write_buf);
  return success;
}

static bool install_firmware_update(void) {
  FILE *file = fopen(update_path, "rb");
  if (file == NULL) {
    ESP_LOGI(TAG, "No hay actualizacion en %s", update_path);
    return false;
  }

  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    ESP_LOGE(TAG, "No hay particion OTA disponible; firmware activo: %s",
             running_partition != NULL ? running_partition->label : "desconocido");
    ESP_LOGE(TAG, "Debe reflashearse la tabla de particiones OTA con 'idf.py flash'");
    fclose(file);
    return false;
  }

  uint8_t *buffer = (uint8_t *)malloc(4096);
  if (buffer == NULL) {
    ESP_LOGE(TAG, "No se pudo reservar el buffer para la actualizacion OTA");
    fclose(file);
    return false;
  }

  esp_ota_handle_t ota_handle = 0;
  esp_err_t error = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "No se pudo iniciar OTA: %s", esp_err_to_name(error));
    free(buffer);
    fclose(file);
    return false;
  }

  bool success = true;
  size_t bytes_read;
  while ((bytes_read = fread(buffer, 1, 4096, file)) > 0) {
    error = esp_ota_write(ota_handle, buffer, bytes_read);
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Error escribiendo firmware OTA: %s", esp_err_to_name(error));
      success = false;
      break;
    }
  }
  if (ferror(file)) {
    ESP_LOGE(TAG, "Error leyendo %s", update_path);
    success = false;
  }
  fclose(file);

  if (success) {
    error = esp_ota_end(ota_handle);
    if (error == ESP_OK) {
      error = esp_ota_set_boot_partition(update_partition);
    }
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Firmware OTA no valido: %s", esp_err_to_name(error));
      success = false;
    }
  } else {
    esp_ota_abort(ota_handle);
  }

  free(buffer);

  if (success) {
    if (remove(update_path) != 0) {
      ESP_LOGW(TAG, "No se pudo borrar %s tras la actualizacion", update_path);
    }
    ESP_LOGI(TAG, "Actualizacion instalada en %s; reiniciando", update_partition->label);
  }
  return success;
}

static void usb_update_task(void *arg) {
  (void)arg;

  while (!hardware_usb_msc_detached()) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_LOGI(TAG, "USB desconectado; recuperando la FAT para buscar firmware");
  if (hardware_exit_usb_msc() == ESP_OK) {
    modo_usb = false;
    hardware_oled_show_message("OTA", "VERIFICANDO");
    if (install_firmware_update()) {
      esp_restart();
    }
  }

  ESP_LOGI(TAG, "Sin actualizacion valida; reiniciando en modo logger");
  esp_restart();
}

static void startup_mode_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(5000));

  if (cabecera_recibida || modo_usb) {
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGW(TAG, "No se recibio la cabecera en 5 segundos; cambiando a USB");
  transmitiendo_archivo = true;
  modo_usb = true;
  if (flush_pending_log() && hardware_enter_usb_msc() == ESP_OK) {
    hardware_oled_hide_battery();
    hardware_oled_show_message("USB", "CONECTE USB");
    ESP_LOGI(TAG, "Modo USB activo; el logger dejo de acceder a la FAT");
    if (xTaskCreate(usb_update_task, "usb_update_task", 4096, NULL, 4, NULL) != pdPASS) {
      ESP_LOGE(TAG, "No se pudo crear la tarea de actualizacion USB");
    }
  } else {
    modo_usb = false;
    transmitiendo_archivo = false;
    ESP_LOGE(TAG, "No se pudo cambiar automaticamente al modo USB");
  }

  vTaskDelete(NULL);
}

static bool ensure_log_file(void) {
  FILE *file = fopen(log_path, "a");
  if (file == NULL) {
    ESP_LOGE(TAG_FLASH, "No se pudo crear o abrir %s: errno=%d", log_path, errno);
    return false;
  }

  if (fclose(file) != 0) {
    ESP_LOGE(TAG_FLASH, "No se pudo cerrar %s: errno=%d", log_path, errno);
    return false;
  }

  ESP_LOGI(TAG_FLASH, "Archivo de log listo: %s", log_path);
  return true;
}

static bool ensure_root_readme(void) {
  const char *readme_path = "/archivos/README.md";
  FILE *existing = fopen(readme_path, "r");
  if (existing != NULL) {
    fclose(existing);
    ESP_LOGI(TAG_FLASH, "README ya existe en la FAT: %s", readme_path);
    return true;
  }

  FILE *file = fopen(readme_path, "w");
  if (file == NULL) {
    ESP_LOGE(TAG_FLASH, "No se pudo crear el README inicial en %s: errno=%d", readme_path, errno);
    return false;
  }

  const char *content =
      "# Datalogger\n\n"
      "Este archivo se crea automaticamente al inicializar la particion FAT.\n"
      "La aplicacion puede guardar aqui logs y datos del sistema.\n";

  size_t written = fwrite(content, 1, strlen(content), file);
  if (written != strlen(content)) {
    ESP_LOGE(TAG_FLASH, "No se pudo escribir el contenido del README inicial en %s", readme_path);
    fclose(file);
    return false;
  }

  if (fflush(file) != 0 || fclose(file) != 0) {
    ESP_LOGE(TAG_FLASH, "No se pudo cerrar correctamente %s", readme_path);
    return false;
  }

  ESP_LOGI(TAG_FLASH, "README inicial creado en la FAT: %s", readme_path);
  return true;
}

// Muestra en el OLED la pérdida de datos UART después de una secuencia activa.
static void secuencia_estado_task(void *arg) {
  bool aviso_sin_datos = false;

  while (!modo_usb) {
    if (secuencia_activa) {
      TickType_t ahora = xTaskGetTickCount();
      TickType_t silencio = ahora - ultima_recepcion_secuencia;
      bool error = silencio > pdMS_TO_TICKS(30000);
      if (error && !aviso_sin_datos) {
        hardware_oled_show_message("AVISO", "SIN DATOS UART");
        ESP_LOGE(TAG, "Sin datos UART durante mas de 30 segundos.");
        aviso_sin_datos = true;
      } else if (!error) {
        aviso_sin_datos = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Tareas Principales
// ---------------------------------------------------------------------------

// Mide la batería y deja el valor en la terminal de depuración cada 10 segundos.
static void battery_voltage_task(void *arg) {
  while (!modo_usb) {
    int battery_voltage_mv = hardware_read_battery_voltage_mv();
    if (!modo_usb && battery_voltage_mv >= 0) {
      ESP_LOGI(TAG, "Voltaje de bateria: %d mV (%.2f V)", battery_voltage_mv, battery_voltage_mv / 1000.0f);
      hardware_oled_show_battery(battery_voltage_mv);
      if (battery_voltage_mv < BATTERY_LOW_AMARILLO_MV && battery_voltage_mv >= BATTERY_LOW_ROJO_MV) {
        ESP_LOGW(TAG, "Bateria baja: aviso mostrado en OLED");
      } else if (battery_voltage_mv < BATTERY_LOW_ROJO_MV) {
        ESP_LOGW(TAG, "Bateria muy baja: aviso mostrado en OLED");
      }
    } else {
      ESP_LOGE(TAG, "No se pudo leer el voltaje de bateria");
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  vTaskDelete(NULL);
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
  bool alerta_tecnologias_mostrada = false;

  char overlap_buf[64] = {0};
  size_t overlap_len = 0;

  while (!modo_usb) {
    int rxBytes = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));

    if (rxBytes > 0) {
      ESP_LOGI(TAG, "UART RX: %d bytes", rxBytes);
      procesar_running(data, rxBytes);

      // flag que evita que se guarde o procese el contenido si se está atendiendo
      // un comando del usuario o una transmisión de datos desde la memoria.
      bool ignorar_grabacion = false;
      size_t total_stream_len = overlap_len + rxBytes;

      memcpy(stream_buf, overlap_buf, overlap_len);
      memcpy(stream_buf + overlap_len, data, rxBytes);
      stream_buf[total_stream_len] = '\0';

      // Verificar si se ha recibido la cabecera de "Alert Technologies".
      // El aviso permanece hasta que comienza el procesamiento de tramas.
      if (!alerta_tecnologias_mostrada && alerta_tecnologias_recibida(stream_buf)) {
        hardware_oled_show_message("CABECERA RECIBIDA\n\r", "ESPERANDO TARADO");
        alerta_tecnologias_mostrada = true;
        cabecera_recibida = true;
        ESP_LOGI(TAG, "CABECERA DE CELDA RECIBIDA\n\rESPERANDO TARADO");
      }

      if (alerta_tecnologias_mostrada && estado_grabado == 0) {
        hardware_oled_update_uart_preview(data, rxBytes);
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

      // 2. Almacenamiento y procesamiento de datos
      if (!ignorar_grabacion && !transmitiendo_archivo) {
        if (estado_grabado == 0) {
          char *match = (char *)memmem(stream_buf, total_stream_len, target, target_len);

          if (match != NULL) {
            size_t stream_match_idx = match - stream_buf;
            size_t data_match_start_idx = stream_match_idx > overlap_len
                              ? stream_match_idx - overlap_len
                              : 0;
            size_t data_match_end_idx =
                (stream_match_idx >= overlap_len) ? (stream_match_idx - overlap_len + target_len) : (target_len - (overlap_len - stream_match_idx));

            // Transición a la captura continua de tramas

            ESP_LOGI(TAG, "Cabecera detectada. Iniciando captura de tramas...");
            if (data_match_start_idx > 0 &&
                !ring_buffer_write_all(data, data_match_start_idx)) {
              ESP_LOGE(TAG, "Datos previos al loop descartados por falta de espacio en RAM.");
            }
            estado_grabado = 2;
            hardware_oled_clear_uart_preview();
            reset_capture_state();

            // Procesar el resto del buffer inmediatamente en la máquina de estados
            if (rxBytes > data_match_end_idx) {
              hardware_oled_update_loop_preview(data + data_match_end_idx, rxBytes - data_match_end_idx);
              procesar_loop_secuencia(data + data_match_end_idx, rxBytes - data_match_end_idx);
            }
          } else {
            if (!ring_buffer_write_all(data, rxBytes))
              ESP_LOGE(TAG, "Datos UART descartados parcialmente por falta de espacio en RAM.");
          }
        } else if (estado_grabado == 2) {
          // Captura continua activa: el parser guarda las tramas compactadas.
          hardware_oled_update_loop_preview(data, rxBytes);
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

  if (!ensure_log_file() || !ensure_root_readme()) {
    ESP_LOGE(TAG, "Almacenamiento no disponible; no se iniciaran las tareas del datalogger");
    return;
  }

  if (install_firmware_update()) {
    esp_restart();
  }

  // Creación de tareas FreeRTOS
  if (xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, 5, NULL) != pdPASS ||
      xTaskCreate(flash_writer_task, "flash_writer_task", 4096, NULL, 4, NULL) != pdPASS ||
      xTaskCreate(secuencia_estado_task, "secuencia_estado_task", 2048, NULL, 3, NULL) != pdPASS ||
      xTaskCreate(battery_voltage_task, "battery_voltage_task", 2048, NULL, 3, NULL) != pdPASS ||
      xTaskCreate(startup_mode_task, "startup_mode_task", 2048, NULL, 3, NULL) != pdPASS) {
    ESP_LOGE(TAG, "No se pudieron crear todas las tareas del sistema.");
    return;
  }

  uart_write_bytes(UART_PORT_NUM, DL_HEADER2, strlen(DL_HEADER2));

  vTaskDelete(NULL);
}