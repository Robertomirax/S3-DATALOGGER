//-------------------------------- main.c --------------------------------

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware.h"
#include <ctype.h>
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
static const char *update_path = "/archivos/S3-DATALOGGER.bin";
static const char DL_HEADER2[] = "\r\n\r\nAlert Technologies\r\nDATALOGGER "
                                 "VER1.04\r\n\nMemory Used...08%\r\n";

static bool flush_pending_log(void);
static bool validar_linea_cliente(const char *line);

static void log_invert_bytes(uint8_t *data, size_t len) {
  if (data == NULL) {
    return;
  }

  for (size_t i = 0; i < len; i++) {
    data[i] = (uint8_t)~data[i];
  }
}

static bool log_write_inverted(FILE *file, const uint8_t *data, size_t len) {
  if (file == NULL || data == NULL || len == 0) {
    return true;
  }

  uint8_t *encoded = (uint8_t *)malloc(len);
  if (encoded == NULL) {
    return false;
  }

  memcpy(encoded, data, len);
  log_invert_bytes(encoded, len);
  size_t written = fwrite(encoded, 1, len, file);
  free(encoded);
  return written == len;
}

static void log_dump_plain_text(const uint8_t *data, size_t len) {
  if (data == NULL || len == 0) {
    return;
  }

  char preview[129];
  size_t offset = 0;
  while (offset < len) {
    size_t chunk_len = len - offset;
    if (chunk_len > 64) {
      chunk_len = 64;
    }

    size_t out = 0;
    for (size_t i = 0; i < chunk_len && out < sizeof(preview) - 1; i++) {
      uint8_t value = data[offset + i];
      if (value == '\r') {
        preview[out++] = '\\';
        if (out < sizeof(preview) - 1) {
          preview[out++] = 'r';
        }
      } else if (value == '\n') {
        preview[out++] = '\\';
        if (out < sizeof(preview) - 1) {
          preview[out++] = 'n';
        }
      } else if (value >= 0x20 && value <= 0x7E) {
        preview[out++] = (char)value;
      } else {
        preview[out++] = '.';
      }
    }

    preview[out] = '\0';
    ESP_LOGI(TAG_FLASH, "Datos escritos [%u..%u]: %s", (unsigned int)offset, (unsigned int)(offset + chunk_len - 1), preview);
    offset += chunk_len;
  }
}

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

// Comprueba que el ring buffer esta listo para usarse.
// Separar esta comprobacion evita que las tareas intenten tomar un mutex nulo
// durante un fallo de inicializacion y permite abortar la operacion limpiamente.
static bool ring_buffer_ready(void) { return rb.mutex != NULL; }

// Inicializa indices, contador y mutex del buffer circular compartido.
// El buffer empieza vacio y el mutex serializa al receptor UART, al escritor
// de flash y a la tarea de transmision cuando acceden a los mismos bytes.
static void ring_buffer_init(void) {
  rb.head = 0;
  rb.tail = 0;
  rb.count = 0;
  rb.mutex = xSemaphoreCreateMutex();
  if (rb.mutex == NULL) {
    ESP_LOGE(TAG, "No se pudo crear el mutex del ring buffer.");
  }
}

// Inserta tantos bytes como quepan en el ring buffer bajo proteccion del mutex.
// No sobrescribe datos pendientes: cuando se llena, devuelve la cantidad real
// escrita para que el llamador pueda reintentar o registrar la perdida.
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

// Garantiza la insercion de un bloque completo o informa que no fue posible.
// Reintenta durante cinco segundos porque el escritor de flash puede liberar
// espacio entre llamadas; asi se evita partir una trama compactada a medias.
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

// Copia bytes pendientes sin mover la cola del ring buffer.
// La operacion permite inspeccionar o escribir un bloque y solo retirarlo
// despues de confirmar que la operacion externa tuvo exito.
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

// Confirma que una cantidad de bytes ya fue procesada y avanza la cola.
// Limita la eliminacion al contenido real para mantener consistente el
// contador aunque el llamador solicite mas bytes de los disponibles.
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

// Descarta todos los bytes pendientes y reinicia los indices del ring buffer.
// Se usa al confirmar el comando de autodiagnostico despues de truncar el
// archivo, dejando RAM y almacenamiento persistente en el mismo punto.
// Obtiene de forma segura la cantidad de bytes que aun esperan persistencia.
// El valor se toma bajo mutex para que no quede a mitad de una escritura del
// receptor o de una retirada realizada por otra tarea.
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
// Estas variables coordinan la recepción de datos y la activación de secuencia.

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
static const char running_header[] = "\"Running:\"\r\n\"   seq #\",\"ld cella\",\"     dac\",\"    temp\",\"    tare\"";
static char running_context[sizeof(running_header)];
static size_t running_context_len = 0;

// Conserva el numero que acaba de terminar en el flujo UART.
//
// `procesar_running` recibe datos por bloques y puede encontrar un numero
// dividido entre dos bloques. Esta rutina copia el acumulador temporal a la
// variable estable que se usara cuando aparezca el encabezado `Running:`.
// Si no hay caracteres acumulados, no modifica el ultimo valor valido.
static void guardar_valor_numerico_actual(void) {
  if (valor_numerico_len == 0)
    return;

  memcpy(ultimo_valor_numerico, valor_numerico_actual, valor_numerico_len);
  ultimo_valor_numerico[valor_numerico_len] = '\0';
  ultimo_valor_len = valor_numerico_len;
  valor_numerico_len = 0;
}

// Busca el encabezado completo `Running:` y actualiza la tara del OLED.
//
// Mantiene una ventana de contexto para reconocer el encabezado aunque este
// llegue partido entre varias lecturas UART. Al mismo tiempo detecta numeros
// precedentes y, cuando la secuencia coincide, convierte el ultimo numero a
// porcentaje de tara antes de enviarlo a la capa de hardware.
static void procesar_running(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];

    if (isdigit((unsigned char)c) || ((c == '-' || c == '+' || c == '.') && valor_numerico_len == 0)) {
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

static const char taring_header[] = "\"Taring load cell a:\"\r\n\"    pass\",\" ld cell\",\"     dac\"";
static char taring_context[sizeof(taring_header)];
static size_t taring_context_len = 0;
static bool taring_filas_activas = false;
static char taring_line[48];
static size_t taring_line_len = 0;

// Reinicia el detector de la tabla "Taring load cell a:" y oculta su fila
// del OLED. Se usa al recibir una nueva cabecera y al comenzar el loop.
static void reset_taring_state(void) {
  taring_context_len = 0;
  taring_filas_activas = false;
  taring_line_len = 0;
  hardware_oled_clear_taring();
}

// Detecta la tabla "Taring load cell a:" y muestra en el OLED la ultima fila
// recibida (pass, ld cell, dac) mientras se espera el fin del tarado.
//
// Reutiliza el patron de ventana deslizante de `procesar_running` para
// reconocer el encabezado aunque llegue partido entre bloques UART. Una vez
// dentro de la tabla acumula cada linea y la interpreta como una fila de tres
// valores separados por comas; una linea que no comienza con un digito
// (linea en blanco o el encabezado de "Running:") cierra la tabla.
static void procesar_taring(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];

    if (!taring_filas_activas) {
      if (taring_context_len == sizeof(taring_context) - 1) {
        memmove(taring_context, taring_context + 1, taring_context_len - 1);
        taring_context_len--;
      }
      taring_context[taring_context_len++] = c;
      taring_context[taring_context_len] = '\0';

      if (strstr(taring_context, taring_header) != NULL) {
        ESP_LOGI(TAG, "Cabecera de tarado detectada. Mostrando filas en OLED...");
        taring_filas_activas = true;
        taring_context_len = 0;
        taring_line_len = 0;
      }
      continue;
    }

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      taring_line[taring_line_len] = '\0';

      size_t idx = 0;
      while (taring_line[idx] == ' ' || taring_line[idx] == '\t') {
        idx++;
      }

      if (taring_line[idx] == '\0') {
        // Linea en blanco: puede ser el propio salto del encabezado o el
        // separador antes de "Running:". No cierra la tabla por si sola.
      } else if (!isdigit((unsigned char)taring_line[idx])) {
        ESP_LOGI(TAG, "Fin de la tabla de tarado.");
        taring_filas_activas = false;
      } else {
        int pass_count = 0, ld_cell = 0, dac = 0;
        if (sscanf(taring_line + idx, "%d,%d,%d", &pass_count, &ld_cell, &dac) == 3) {
          ESP_LOGI(TAG, "Fila de tarado: pass=%d ld_cell=%d dac=%d", pass_count, ld_cell, dac);
          hardware_oled_show_taring(pass_count, ld_cell, dac);
        } else {
          ESP_LOGW(TAG, "Fila de tarado no reconocida: '%s'", taring_line + idx);
        }
      }

      taring_line_len = 0;
      continue;
    }

    if (taring_line_len < sizeof(taring_line) - 1) {
      taring_line[taring_line_len++] = c;
    }
  }
}

// Borra el contenido y los indicadores asociados a la linea en construccion.
// Se llama al comenzar una nueva trama para evitar que datos de la anterior
// alteren el resultado del parser o la posicion del indicador de tara.
static void reset_line_buffer(void) {
  line_idx = 0;
  pos_flag_tara = 0;
  line_overflow = false;
  memset(line_buf, 0, sizeof(line_buf));
}

// Devuelve la maquina de estados a su estado inicial de espera de CRLF.
// Tambien reinicia el byte anterior y el buffer de linea, por lo que puede
// usarse tanto al arrancar como despues de borrar la memoria capturada.
static void reset_capture_state(void) {
  subestado_loop = WAIT_CRLF_1;
  last_byte = 0x00;
  linea = 0;
  reset_line_buffer();
}

// Agrega un caracter a la linea compactada si queda espacio para el terminador.
// Cuando la entrada excede el buffer no escribe fuera de los limites: marca
// `line_overflow` para que la trama completa pueda descartarse al finalizar.
static void append_char_to_line(char c) {
  if (line_idx < sizeof(line_buf) - 1) {
    line_buf[line_idx++] = c;
    line_buf[line_idx] = '\0';
  } else {
    line_overflow = true;
  }
}

// Agrega una cadena completa al buffer de linea con la misma proteccion de
// limites que `append_char_to_line`. La marca de overflow se conserva para
// que el parser no guarde una linea truncada como si fuera valida.
static void append_str_to_line(const char *str) {
  while (*str && (line_idx < sizeof(line_buf) - 1)) {
    line_buf[line_idx++] = *str++;
  }
  line_buf[line_idx] = '\0';
  if (*str != '\0')
    line_overflow = true;
}

// Detecta la cabecera de identificacion del equipo dentro de un bloque de UART.
// Exige tanto el prefijo de Alert Technologies como el separador de version,
// reduciendo el riesgo de activar el modo logger por una coincidencia parcial.
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

static void quitar_espacios_izquierda(char *text) {
  if (text == NULL) {
    return;
  }

  size_t index = 0;
  while (text[index] == ' ' || text[index] == '\t' || text[index] == '\r' || text[index] == '\n') {
    index++;
  }

  if (index > 0) {
    memmove(text, text + index, strlen(text + index) + 1);
  }
}

static bool validar_linea_cliente(const char *line) {
  if (line == NULL)
    return false;

  char normalized[64];
  snprintf(normalized, sizeof(normalized), "%.63s", line);
  quitar_espacios_izquierda(normalized);

  const unsigned char *p = (const unsigned char *)normalized;
  while (*p != '\0' && isspace(*p)) {
    p++;
  }

  if (*p == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
    p += 3;
  }

  if (strncmp((const char *)p, "C:", 2) != 0) {
    ESP_LOGW(TAG, "Fallo en prefijo C: en '%s' (primer byte=%02X)", line, (unsigned int)p[0]);
    return false;
  }
  p += 2;

  char c_value[9] = {0};
  size_t c_len = 0;
  while (*p != '\0' && !isspace((unsigned char)*p) && c_len < sizeof(c_value) - 1) {
    unsigned char ch = (unsigned char)*p;
    if (!(isalnum(ch))) {
      ESP_LOGW(TAG, "Caracter no alfanumerico en C: '%s' (caracter=%c)", line, ch);
      return false;
    }
    c_value[c_len++] = (char)ch;
    p++;
  }
  c_value[c_len] = '\0';
  if (c_len == 0 || c_len > 8) {
    ESP_LOGW(TAG, "Longitud invalida de C: '%s' (%zu)", line, c_len);
    return false;
  }

  if (!isspace((unsigned char)*p)) {
    ESP_LOGW(TAG, "No hay separador y prefijo T: despues de C: '%s' (siguiente=%02X)", line, (unsigned char)*p);
    return false;
  }
  while (*p != '\0' && isspace((unsigned char)*p)) {
    p++;
  }

  if (strncmp((const char *)p, "T:", 2) != 0) {
    ESP_LOGW(TAG, "Fallo en prefijo T: en '%s'", line);
    return false;
  }
  p += 2;

  char t_value[9] = {0};
  size_t t_len = 0;
  while (*p != '\0' && !isspace((unsigned char)*p) && t_len < sizeof(t_value) - 1) {
    unsigned char ch = (unsigned char)*p;
    if (!(isalnum(ch))) {
      ESP_LOGW(TAG, "Caracter no alfanumerico en T: '%s' (caracter=%c)", line, ch);
      return false;
    }
    t_value[t_len++] = (char)ch;
    p++;
  }
  t_value[t_len] = '\0';
  if (t_len == 0 || t_len > 8) {
    ESP_LOGW(TAG, "Longitud invalida de T: '%s' (%zu)", line, t_len);
    return false;
  }

  while (*p != '\0' && isspace((unsigned char)*p)) {
    p++;
  }

  if (*p != '\0') {
    ESP_LOGW(TAG, "Caracteres extra despues de T: '%s'", line);
    return false;
  }

  ESP_LOGI(TAG, "Cliente validado: C='%s' T='%s'", c_value, t_value);
  return true;
}

static bool cargar_linea_cliente_desde_log(void) {
  if (ring_buffer_get_count() > 0) {
    flush_pending_log();
  }

  // El log se guarda con los bits de cada byte invertidos; hay que decodificar
  // cada bloque leido antes de poder interpretarlo como texto y detectar CRLF.
  FILE *file = fopen(log_path, "rb");
  if (file == NULL) {
    ESP_LOGE(TAG, "No se pudo abrir %s para leer la linea del cliente.", log_path);
    hardware_oled_show_client_missing();
    return false;
  }

  bool encontrado = false;
  bool valido = false;
  char line[128];
  size_t line_len = 0;
  uint8_t chunk[64];
  size_t bytes_read;

  while (!valido && (bytes_read = fread(chunk, 1, sizeof(chunk), file)) > 0) {
    log_invert_bytes(chunk, bytes_read);

    for (size_t i = 0; i < bytes_read && !valido; i++) {
      char c = (char)chunk[i];

      if (c == '\n' || c == '\r') {
        if (line_len == 0) {
          continue;
        }
        while (line_len > 0 && (line[line_len - 1] == ' ' || line[line_len - 1] == '\t')) {
          line_len--;
        }
        line[line_len] = '\0';

        if (line_len == 0) {
          continue;
        }

        ESP_LOGI(TAG, "LINEA CRUDA LEIDA desde %s: '%s'", log_path, line);
        encontrado = true;

        if (validar_linea_cliente(line)) {
          char client_display[32];
          snprintf(client_display, sizeof(client_display), "%.31s", line);
          quitar_espacios_izquierda(client_display);

          hardware_oled_show_client_line(client_display);
          ESP_LOGI(TAG, "Linea del cliente valida: '%s'", client_display);
          valido = true;
        } else {
          ESP_LOGW(TAG, "Linea del cliente invalida: '%s'", line);
        }

        line_len = 0;
        continue;
      }

      if (line_len < sizeof(line) - 1) {
        line[line_len++] = c;
      }
    }
  }

  fclose(file);

  if (valido) {
    return true;
  }

  if (!encontrado) {
    ESP_LOGW(TAG, "No se encontraron lineas en %s para validar el cliente.", log_path);
  }
  hardware_oled_show_client_missing();
  return false;
}

// ---------------------------------------------------------------------------
// Procesador byte a byte
// ---------------------------------------------------------------------------
// Consume bytes UART y reconstruye una trama compactada mediante una maquina
// de estados. La rutina espera el CRLF inicial, selecciona los campos que se
// almacenan, elimina ceros segun el formato del equipo y aplica el bit de
// tara. Al terminar una trama actualiza el OLED y la encola en el ring buffer.
// El estado se conserva entre llamadas porque una lectura UART puede terminar
// en mitad de cualquier campo.
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
// Tarea productora de escrituras: traslada datos del ring buffer a la FAT.
// Despierta periodicamente y escribe cuando hay 2 KB pendientes o cuando han
// pasado cinco segundos, manteniendo el mutex del archivo durante toda la
// operacion para coordinarse con el borrado, la transmision y el USB.
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

    if (should_write) {
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

            if (!log_write_inverted(f, write_buf, bytes_to_read)) {
              ESP_LOGE(TAG_FLASH, "Error escribiendo log: %u bytes", (unsigned int)bytes_to_read);
              if (ferror(f)) {
                ESP_LOGE(TAG_FLASH, "Error del flujo de archivo: %d", errno);
              }
              break;
            }

            log_dump_plain_text(write_buf, bytes_to_read);
            ESP_LOGI(TAG_FLASH, "Grabado en flash con bits invertidos (%u bytes)", (unsigned int)bytes_to_read);
            ring_buffer_drop(bytes_to_read);
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

// Fuerza el volcado de todas las tramas pendientes antes de ceder la FAT al
// host USB. Devuelve true solo si el archivo se pudo abrir, sincronizar y el
// ring buffer quedo completamente vacio.
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
        if (bytes_to_read == 0) {
          break;
        }
        if (!log_write_inverted(file, write_buf, bytes_to_read)) {
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

// Instala una imagen OTA encontrada en la particion FAT.
// Lee el binario por bloques, lo escribe en la particion OTA alternativa,
// finaliza la validacion de ESP-IDF y cambia la particion de arranque. El
// archivo solo se elimina despues de completar todos esos pasos correctamente.
static bool install_firmware_update(void) {
  FILE *file = fopen(update_path, "rb");
  if (file == NULL) {
    ESP_LOGI(TAG, "No hay actualizacion en %s", update_path);
    return false;
  }

  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    ESP_LOGE(TAG, "No hay particion OTA disponible; firmware activo: %s", running_partition != NULL ? running_partition->label : "desconocido");
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

// Espera la desconexion fisica del pendrive y recupera el control de la FAT.
// Una vez desmontado el USB busca una imagen OTA; si la instala, reinicia para
// arrancar el nuevo firmware, y si no, reinicia conservando el firmware actual.
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

// Decide el modo de arranque cuando no se recibe la cabecera de la celda.
// Tras cinco segundos vacia el log pendiente y entrega la particion al USB;
// si la cabecera ya llego, termina sin interferir con el modo logger.
static void startup_mode_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(5000));

  if (cabecera_recibida || modo_usb) {
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGW(TAG, "No se recibio la cabecera en 5 segundos; cambiando a USB");
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
    ESP_LOGE(TAG, "No se pudo cambiar automaticamente al modo USB");
  }

  vTaskDelete(NULL);
}

// Verifica que el archivo de log pueda crearse o abrirse en la FAT montada.
// No escribe datos; sirve como comprobacion temprana antes de crear tareas que
// dependan del almacenamiento.
static bool ensure_log_file(void) {
  FILE *file = fopen(log_path, "rb");
  if (file != NULL) {
    fclose(file);
    ESP_LOGI(TAG_FLASH, "Log existente conservado en texto plano: %s", log_path);
    return true;
  }

  FILE *fresh = fopen(log_path, "a");
  if (fresh == NULL) {
    ESP_LOGE(TAG_FLASH, "No se pudo crear o abrir %s: errno=%d", log_path, errno);
    return false;
  }

  if (fclose(fresh) != 0) {
    ESP_LOGE(TAG_FLASH, "No se pudo cerrar %s: errno=%d", log_path, errno);
    return false;
  }

  ESP_LOGI(TAG_FLASH, "Archivo de log listo: %s", log_path);
  return true;
}

// Crea el README inicial de la particion FAT si aun no existe.
// La operacion es deliberadamente idempotente para no reemplazar el archivo
// que el usuario pueda haber dejado en el volumen USB.
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

  const char *content = "# Datalogger\n\n"
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

// Supervisa el silencio del UART una vez iniciada la secuencia.
// Muestra un aviso unico cuando transcurren mas de 30 segundos sin datos y lo
// habilita de nuevo cuando la recepcion se reanuda.
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

// Tarea periodica de supervision de bateria.
// Lee el ADC cada cinco segundos, publica el voltaje en el log y actualiza la
// linea correspondiente del OLED. La tarea se detiene al entrar en modo USB.
static void battery_voltage_task(void *arg) {
  while (!modo_usb) {
    int battery_voltage_mv = hardware_read_battery_voltage_mv();
    if (!modo_usb && battery_voltage_mv >= 0) {
      ESP_LOGI(TAG, "Voltaje de bateria: %d mV (%.2f V)", battery_voltage_mv, battery_voltage_mv / 1000.0f);
      hardware_oled_show_battery(battery_voltage_mv);

    } else {
      ESP_LOGE(TAG, "No se pudo leer el voltaje de bateria");
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }

  vTaskDelete(NULL);
}

// Tarea principal de recepcion UART y coordinacion del datalogger.
// Conserva datos previos al loop, reconoce la cabecera de captura, atiende los
// el flujo continuo a la maquina de estados y mantiene solapamiento entre
// bloques UART. Libera sus buffers al cambiar a USB.
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
  bool cliente_datos_validos = false;

  while (!modo_usb) {
    int rxBytes = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));

    if (rxBytes > 0) {
      ESP_LOGI(TAG, "UART RX: %d bytes", rxBytes);
      procesar_running(data, rxBytes);
      procesar_taring(data, rxBytes);

      bool ignorar_grabacion = false;
      size_t total_stream_len = overlap_len + rxBytes;

      memcpy(stream_buf, overlap_buf, overlap_len);
      memcpy(stream_buf + overlap_len, data, rxBytes);
      stream_buf[total_stream_len] = '\0';

      if (alerta_tecnologias_mostrada && !cliente_datos_validos) {
        hardware_oled_show_client_missing();
        overlap_len = 0;
        memset(overlap_buf, 0, sizeof(overlap_buf));
        ignorar_grabacion = true;
        continue;
      }

      // Verificar si se ha recibido la cabecera de "Alert Technologies".
      // El aviso permanece hasta que comienza el procesamiento de tramas.
      if (!alerta_tecnologias_mostrada && alerta_tecnologias_recibida(stream_buf)) {
        alerta_tecnologias_mostrada = true;
        cabecera_recibida = true;
        cliente_datos_validos = cargar_linea_cliente_desde_log();
        if (!cliente_datos_validos) {
          ESP_LOGE(TAG, "FALTAN LOS DATOS DEL CLIENTE en %s; no se registraran los datos recibidos.", log_path);
          hardware_oled_show_client_missing();
          overlap_len = 0;
          memset(overlap_buf, 0, sizeof(overlap_buf));
          ignorar_grabacion = true;
          estado_grabado = 0;
          continue;
        }
        hardware_oled_show_message("CABECERA RECIBIDA\n\r", "ESPERANDO TARADO");
        ESP_LOGI(TAG, "CABECERA DE CELDA RECIBIDA\n\rESPERANDO TARADO");
        reset_taring_state();
      }

      // Almacenamiento y procesamiento de datos
      if (!ignorar_grabacion) {
        if (estado_grabado == 0) {
          char *match = (char *)memmem(stream_buf, total_stream_len, target, target_len);

          if (match != NULL) {
            size_t stream_match_idx = match - stream_buf;
            size_t data_match_end_idx =
                (stream_match_idx >= overlap_len) ? (stream_match_idx - overlap_len + target_len) : (target_len - (overlap_len - stream_match_idx));

            // Transición a la captura continua de tramas

            ESP_LOGI(TAG, "Cabecera detectada. Iniciando captura de tramas...");
            // El encabezado de Running tambien forma parte del log. Antes solo
            // se conservaba lo anterior a "seq #", descartando esta fila.
            if (data_match_end_idx > 0 && !ring_buffer_write_all(data, data_match_end_idx)) {
              ESP_LOGE(TAG, "Datos previos al loop descartados por falta de espacio en RAM.");
            }
            estado_grabado = 2;
            hardware_oled_clear_uart_preview();
            reset_taring_state();
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
      if (!ignorar_grabacion) {
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
// Punto de entrada de FreeRTOS para el firmware.
// Crea las primitivas de sincronizacion, monta e inicializa el hardware,
// comprueba el almacenamiento, procesa una OTA pendiente y lanza las tareas
// de recepcion, persistencia, supervision, bateria y seleccion de modo.
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