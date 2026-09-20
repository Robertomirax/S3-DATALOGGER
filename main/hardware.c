//------------------------------ hardware.c --------------------------------
// Implementación de la inicialización del hardware del datalogger.
// Aquí se configura la capa UART, el OLED y el almacenamiento USB.

#include "hardware.h"
#include "driver/gpio.h" // IWYU pragma: keep
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_partition.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"
#include "ff.h"
#include <stddef.h>

#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "HARDWARE";

static adc_oneshot_unit_handle_t battery_adc_handle = NULL;
static adc_cali_handle_t battery_adc_cali_handle = NULL;
static adc_channel_t battery_adc_channel;
static esp_lcd_panel_handle_t oled_panel = NULL;
static uint8_t oled_framebuffer[128 * 64 / 8];
static int oled_battery_voltage_mv = -1;
static bool oled_battery_visible = true;
static float oled_tara_percent = 0.0f;
static bool oled_tara_valid = false;
static char oled_frame_text[128];
static char oled_status_title[64];
static char oled_status_message[128];
static char oled_client_line[32];
static bool oled_client_line_active = false;
static bool oled_client_missing = false;
static char oled_uart_preview[44];
static size_t oled_uart_preview_len = 0;
static bool oled_uart_preview_active = false;
static char oled_loop_preview[66];
static size_t oled_loop_preview_len = 0;
static bool oled_loop_number_started = false;
static bool oled_loop_pending_zero = false;
static bool oled_loop_preview_active = false;
static bool oled_loop_pending_shift = false;
static bool oled_status_active = false;
static bool oled_taring_active = false;
static int oled_taring_pass = 0;
static int oled_taring_ld_cell = 0;
static int oled_taring_dac = 0;
static bool usb_msc_active = false;
static volatile bool usb_msc_detached = false;

static tinyusb_msc_storage_handle_t msc_storage_handle;
static wl_handle_t msc_wl_handle = WL_INVALID_HANDLE;

// Callback de TinyUSB ejecutado cuando cambia el estado del dispositivo USB.
// Solo nos interesa la desconexion: la tarea de actualizacion la consulta para
// saber cuando el host dejo de usar la particion FAT y puede devolversela a la
// aplicacion sin riesgo de acceso simultaneo.
static void usb_event_callback(tinyusb_event_t *event, void *arg) {
  (void)arg;
  if (event != NULL && event->id == TINYUSB_EVENT_DETACHED) {
    usb_msc_detached = true;
  }
}

// Devuelve los cinco bytes de columnas que forman un caracter de la fuente
// monocroma del SSD1306. La fuente solo contiene mayusculas, digitos y unos
// signos; por eso convierte minusculas a mayusculas y usa un glifo en blanco
// cuando recibe un caracter fuera del conjunto soportado.
static const uint8_t *oled_glyph(char character) {
  if (character >= 'a' && character <= 'z') {
    character = (char)(character - 'a' + 'A');
  }

  static const char characters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .:-%";
  static const uint8_t glyphs[][5] = {
      {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
      {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
      {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
      {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
      {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
      {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
      {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
      {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
      {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x3E, 0x45, 0x49, 0x51, 0x3E},
      {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
      {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30},
      {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
      {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x00, 0x36, 0x36, 0x00, 0x00},
      {0x08, 0x08, 0x08, 0x08, 0x08}, {0x63, 0x13, 0x08, 0x64, 0x63},
  };
  for (size_t index = 0; index < sizeof(characters) - 1; index++) {
    if (characters[index] == character) {
      return glyphs[index];
    }
  }
  return glyphs[36];
}

// Dibuja un glifo de 5 columnas en la fila de pixeles indicada, repartiendo
// los bits entre dos paginas cuando la fila no coincide con un multiplo de 8.
// Esto permite un espaciado vertical mas fino que los 8 px de una pagina.
static void oled_draw_glyph_row(const uint8_t *glyph, uint8_t column, uint8_t row) {
  uint8_t page = row / 8;
  uint8_t shift = row % 8;
  for (uint8_t index = 0; index < 5 && column + index < 128; index++) {
    oled_framebuffer[(page * 128) + column + index] |= (uint8_t)(glyph[index] << shift);
    if (shift != 0 && page + 1 < 8) {
      oled_framebuffer[((page + 1) * 128) + column + index] |= (uint8_t)(glyph[index] >> (8 - shift));
    }
  }
}

// Dibuja una cadena en la fila de pixeles indicada del framebuffer del OLED.
// Cada caracter ocupa cinco columnas mas una columna de separacion. La rutina
// limita la escritura al ancho de 128 pixeles para proteger el framebuffer.
static void trim_left_spaces(char *text) {
  if (text == NULL) {
    return;
  }

  size_t index = 0;
  while (text[index] == ' ' || text[index] == '\t' || text[index] == '\r' || text[index] == '\n') {
    index++;
  }

  if ((unsigned char)text[index] == 0xEF && (unsigned char)text[index + 1] == 0xBB &&
      (unsigned char)text[index + 2] == 0xBF) {
    index += 3;
  }

  if (index > 0) {
    memmove(text, text + index, strlen(text + index) + 1);
  }
}

static void oled_draw_text(const char *text, uint8_t column, uint8_t row) {
  if (text == NULL) {
    return;
  }

  while (*text != '\0' && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) {
    text++;
  }

  uint8_t x = column;
  while (*text != '\0' && x < 128) {
    oled_draw_glyph_row(oled_glyph(*text++), x, row);
    x += 6;
  }
}

// Dibuja texto continuo admitiendo CR/LF y salto automatico de linea cada
// 10 px. Se utiliza para mensajes de estado que pueden ocupar varias lineas y
// deja de escribir cuando se sale de los 64 pixeles de alto del panel.
static void oled_draw_wrapped_text(const char *text, uint8_t row) {
  uint8_t column = 0;
  while (*text != '\0' && row < 64) {
    if (*text == '\r') {
      text++;
      continue;
    }
    if (*text == '\n') {
      text++;
      column = 0;
      row += 11;
      continue;
    }

    oled_draw_glyph_row(oled_glyph(*text++), column, row);
    column += 6;
    if (column >= 126) {
      column = 0;
      row += 11;
    }
  }
}

// Compone la primera zona de la pantalla con bateria y tara.
// La tension se muestra en voltios con dos decimales; la tara solo aparece
// cuando el parser ya recibio un valor valido desde la secuencia Running.
static void oled_draw_battery_line(void) {
  char battery_text[22];
  if (oled_battery_voltage_mv >= 0) {
    snprintf(battery_text, sizeof(battery_text), "BAT: %d.%02d V", oled_battery_voltage_mv / 1000,
             (oled_battery_voltage_mv % 1000) / 10);
  } else {
    snprintf(battery_text, sizeof(battery_text), "BAT: -- V");
  }
  oled_draw_text(battery_text, 0, 11);

  if (oled_tara_valid) {
    char tara_text[22];
    snprintf(tara_text, sizeof(tara_text), "TARA = %.0f %%", oled_tara_percent);
    oled_draw_text(tara_text, 0, 22);
  }
}

// Dibuja el identificador de firmware alineado al borde derecho de la pantalla.
// La alineacion se calcula a partir del numero de caracteres para que la
// etiqueta conserve su posicion aunque cambie la version.
static void oled_draw_firmware_line(void) {
  char firmware_text[16];
  int firmware_length = snprintf(firmware_text, sizeof(firmware_text), "FIRM %d", FIRM);
  if (firmware_length <= 0) {
    return;
  }

  int firmware_width = firmware_length * 6;
  uint8_t column = firmware_width < 128 ? (uint8_t)(128 - firmware_width) : 0;
  oled_draw_text(firmware_text, column, 11);
}

// Anexa un caracter a la tercera linea (la que se esta formando) de la vista
// previa del loop. Las dos lineas anteriores ya quedaron fijadas al hacer
// scroll, por eso la escritura siempre ocurre en el ultimo tercio del buffer.
static void oled_loop_append_char(char character) {
  if (oled_loop_preview_len < 21) {
    oled_loop_preview[44 + oled_loop_preview_len++] = character;
  }
}

// Cierra el numero que se estaba acumulando en la vista previa del loop.
// Si se omitio temporalmente un cero inicial, lo restaura solo al terminar el
// numero, preservando la representacion util sin llenar el OLED de ceros.
static void oled_loop_finish_number(void) {
  if (oled_loop_pending_zero) {
    oled_loop_append_char('0');
    oled_loop_pending_zero = false;
  }
  oled_loop_number_started = false;
}

// Extrae hasta cinco grupos numericos de la linea del loop y los compacta.
// Esta transformacion es exclusivamente visual: el log conserva la trama
// completa, mientras que el OLED muestra una version legible en 21 columnas.
static void oled_loop_compact_line(void) {
  char compact_line[22] = {0};
  size_t compact_len = 0;
  size_t number_count = 0;
  bool in_number = false;
  bool pending_space = false;

  for (size_t index = 44; index < 66 && oled_loop_preview[index] != '\0'; index++) {
    char character = oled_loop_preview[index];
    if (character >= '0' && character <= '9') {
      if (!in_number && number_count >= 5) {
        continue;
      }
      if (pending_space && compact_len > 0 && compact_len < sizeof(compact_line) - 1) {
        compact_line[compact_len++] = ' ';
      }
      pending_space = false;
      if (compact_len < sizeof(compact_line) - 1) {
        compact_line[compact_len++] = character;
      }
      if (!in_number) {
        number_count++;
      }
      in_number = true;
    } else if (in_number) {
      pending_space = true;
      in_number = false;
    }
    if (compact_len >= sizeof(compact_line) - 1) {
      break;
    }
  }

  memcpy(oled_loop_preview + 44, compact_line, sizeof(compact_line));
  oled_loop_preview_len = compact_len;
}

// Reconstruye el framebuffer completo a partir del estado actual de la UI.
// Dibuja las capas en orden, borra restos del frame anterior y envia el bitmap
// terminado al controlador SSD1306 en una sola operacion.
static void oled_render_frame(void) {
  memset(oled_framebuffer, 0, sizeof(oled_framebuffer));

  if (oled_client_line_active) {
    oled_draw_text(oled_client_line, 0, 0);
  }

  if (oled_battery_visible) {
    oled_draw_battery_line();
  }
  oled_draw_firmware_line();
  if (oled_status_active) {
    oled_draw_text(oled_status_title, 0, 22);
    oled_draw_wrapped_text(oled_status_message, 33);
    if (oled_taring_active) {
      char taring_header_line[22];
      snprintf(taring_header_line, sizeof(taring_header_line), "PASS LDCELL  DAC");
      oled_draw_text(taring_header_line, 0, 44);

      char taring_values_line[22];
      snprintf(taring_values_line, sizeof(taring_values_line), "%4d %5d %5d", oled_taring_pass, oled_taring_ld_cell, oled_taring_dac);
      oled_draw_text(taring_values_line, 0, 55);
    }
  } else if (!oled_loop_preview_active) {
    oled_draw_wrapped_text(oled_frame_text, 33);
  }
  if (oled_loop_preview_active) {
    oled_draw_text(oled_loop_preview, 0, 33);
    oled_draw_text(oled_loop_preview + 22, 0, 44);
    oled_draw_text(oled_loop_preview + 44, 0, 55);
  }
  if (oled_uart_preview_active) {
    oled_draw_text(oled_uart_preview, 0, 44);
    oled_draw_text(oled_uart_preview + 22, 0, 55);
  }
  ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(oled_panel, 0, 0, 128, 64, oled_framebuffer));
}

void hardware_init_oled(void) {
  // Crea el bus I2C, registra el panel SSD1306 y deja el primer framebuffer
  // visible. Las comprobaciones ESP_ERROR_CHECK detienen el arranque si el
  // controlador no puede configurarse de forma segura.
  const i2c_master_bus_config_t bus_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = OLED_I2C_SDA_GPIO,
      .scl_io_num = OLED_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t bus_handle = NULL;
  esp_err_t error = i2c_new_master_bus(&bus_config, &bus_handle);
  if (error != ESP_OK) {
    ESP_LOGW(TAG, "No se pudo crear el bus I2C del OLED: %s", esp_err_to_name(error));
    return;
  }

  const esp_lcd_panel_io_i2c_config_t io_config = {
      .dev_addr = OLED_I2C_ADDRESS,
      .scl_speed_hz = 400000,
      .control_phase_bytes = 1,
      .dc_bit_offset = 6,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };
  esp_lcd_panel_io_handle_t io_handle = NULL;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus_handle, &io_config, &io_handle));

  const esp_lcd_panel_ssd1306_config_t ssd1306_config = {.height = 64, .contrast = 255};
  const esp_lcd_panel_dev_config_t panel_config = {
      .bits_per_pixel = 1,
      .reset_gpio_num = -1,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
      .vendor_config = (void *)&ssd1306_config,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &oled_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(oled_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(oled_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(oled_panel, true, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(oled_panel, true));
  oled_render_frame();
  ESP_LOGI(TAG, "OLED SSD1306 inicializado en I2C 0x%02X", OLED_I2C_ADDRESS);
}

void hardware_oled_show_battery(int voltage_mv) {
  // Actualiza el valor de bateria almacenado y vuelve a dibujar la pantalla.
  // No intenta pintar antes de inicializar el panel ni cuando la bateria esta
  // oculta temporalmente por el modo USB.
  if (oled_panel == NULL || !oled_battery_visible) {
    return;
  }
  oled_battery_voltage_mv = voltage_mv;
  oled_render_frame();
}

void hardware_oled_hide_battery(void) {
  // Desactiva la capa visual de bateria y conserva el resto de la interfaz.
  // Se usa al ceder la FAT al host USB para indicar el cambio de modo.
  oled_battery_visible = false;
  if (oled_panel != NULL) {
    oled_render_frame();
  }
}

void hardware_oled_show_message(const char *title, const char *message) {
  // Reemplaza la zona central del OLED por un titulo y un mensaje de estado.
  // Copia las cadenas con limite para que una entrada externa no desborde el
  // estado persistente de la interfaz.
  if (oled_panel == NULL) {
    return;
  }

  snprintf(oled_status_title, sizeof(oled_status_title), "%s", title != NULL ? title : "");
  snprintf(oled_status_message, sizeof(oled_status_message), "%s", message != NULL ? message : "");
  oled_status_active = true;
  oled_render_frame();
}

void hardware_oled_show_tara(float tara_percent) {
  // Registra una tara calculada por el parser y la muestra en pantalla.
  // Tambien retira el mensaje temporal de estado para volver a la vista normal.
  if (oled_panel == NULL) {
    return;
  }

  oled_tara_percent = tara_percent;
  oled_tara_valid = true;
  oled_status_active = false;
  oled_render_frame();
}

void hardware_oled_show_frame(const char *frame) {
  // Guarda y muestra la ultima trama compactada recibida del equipo.
  // Al mostrar una trama se desactiva la vista previa UART anterior.
  if (oled_panel == NULL || frame == NULL) {
    return;
  }

  snprintf(oled_frame_text, sizeof(oled_frame_text), "%s", frame);
  oled_uart_preview_active = false;
  oled_status_active = false;
  oled_render_frame();
}

void hardware_oled_show_client_line(const char *line) {
  if (oled_panel == NULL || line == NULL) {
    return;
  }

  char cleaned[32] = {0};
  snprintf(cleaned, sizeof(cleaned), "%.31s", line);
  trim_left_spaces(cleaned);

  snprintf(oled_client_line, sizeof(oled_client_line), "%s", cleaned);
  oled_client_line_active = true;
  oled_client_missing = false;
  //ESP_LOGI("OLED", "Mostrando linea de cliente:-%s-", cleaned);
  oled_render_frame();
}

void hardware_oled_show_client_missing(void) {
  if (oled_panel == NULL) {
    return;
  }

  snprintf(oled_client_line, sizeof(oled_client_line), "FALTAN DATOS CLIENTE");
  oled_client_line_active = true;
  oled_client_missing = true;
  oled_render_frame();
}

void hardware_oled_update_uart_preview(const uint8_t *data, size_t len) {
  // Alimenta la vista previa de la cabecera UART antes de iniciar el loop.
  // Filtra controles, normaliza tabuladores y conserva solo las dos ultimas
  // lineas que caben en el OLED; no modifica los datos que se guardan en FAT.
  if (oled_panel == NULL || data == NULL || len == 0) {
    return;
  }

  for (size_t index = 0; index < len; index++) {
    char character = (char)data[index];
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      memcpy(oled_uart_preview, oled_uart_preview + 22, 22);
      memset(oled_uart_preview + 22, 0, 22);
      oled_uart_preview_len = 0;
      continue;
    }
    if (character == '\t') {
      character = ' ';
    } else if ((unsigned char)character < 0x20 || (unsigned char)character > 0x7E) {
      continue;
    }

    // Compacta los separadores solo para que la vista previa quepa en el OLED.
    if (character == ' ') {
      if (oled_uart_preview_len == 0 ||
          oled_uart_preview[22 + oled_uart_preview_len - 1] == ' ') {
        continue;
      }
    }

    if (oled_uart_preview_len == 21) {
      memmove(oled_uart_preview + 22, oled_uart_preview + 23, 20);
      oled_uart_preview_len--;
    }
    oled_uart_preview[22 + oled_uart_preview_len++] = character;
    oled_uart_preview[22 + oled_uart_preview_len] = '\0';
  }

  oled_uart_preview[21] = '\0';
  oled_uart_preview_active = true;
  oled_render_frame();
}

void hardware_oled_show_taring(int pass_count, int ld_cell, int dac) {
  // Refleja en pantalla la fila mas reciente de la tabla de tarado mientras
  // el equipo sigue esperando la cabecera "Running:".
  if (oled_panel == NULL) {
    return;
  }

  oled_taring_active = true;
  oled_taring_pass = pass_count;
  oled_taring_ld_cell = ld_cell;
  oled_taring_dac = dac;
  oled_render_frame();
}

void hardware_oled_clear_taring(void) {
  oled_taring_active = false;
  if (oled_panel != NULL) {
    oled_render_frame();
  }
}

void hardware_oled_clear_uart_preview(void) {
  // Elimina las vistas previas de cabecera y loop al comenzar la captura.
  // Reinicia tambien sus contadores para que la siguiente sesion no herede
  // caracteres de la sesion anterior.
  if (oled_panel == NULL) {
    return;
  }

  oled_uart_preview_active = false;
  oled_uart_preview_len = 0;
  oled_uart_preview[0] = '\0';
  oled_loop_preview_active = false;
  oled_loop_preview_len = 0;
  oled_loop_preview[0] = '\0';
  oled_loop_pending_shift = false;
  oled_render_frame();
}

void hardware_oled_update_loop_preview(const uint8_t *data, size_t len) {
  // Actualiza la vista previa de las tramas que ya estan entrando al loop.
  // Compacta numeros y separadores para adaptarlos a 21 columnas, manteniendo
  // intacto el flujo original que procesa `main.c`.
  if (oled_panel == NULL || data == NULL || len == 0) {
    return;
  }

  for (size_t index = 0; index < len; index++) {
    char character = (char)data[index];
    if (character == '\r') {
      continue;
    }
    if (character == '\n') {
      oled_loop_finish_number();
      oled_loop_compact_line();
      oled_loop_pending_shift = true;
      continue;
    }
    if ((unsigned char)character < 0x20 || (unsigned char)character > 0x7E) {
      continue;
    }

    // La linea recien cerrada permanece visible hasta que llega el primer
    // caracter de la siguiente; recien ahi se hace scroll hacia arriba.
    if (oled_loop_pending_shift) {
      memmove(oled_loop_preview, oled_loop_preview + 22, 44);
      memset(oled_loop_preview + 44, 0, 22);
      oled_loop_preview_len = 0;
      oled_loop_pending_shift = false;
    }

    if (character == ' ') {
      oled_loop_finish_number();
      if (oled_loop_preview_len == 0 || oled_loop_preview[44 + oled_loop_preview_len - 1] == ' ') {
        continue;
      }
      oled_loop_append_char(' ');
      continue;
    }

    if (character >= '0' && character <= '9') {
      if (character == '0' && !oled_loop_number_started) {
        oled_loop_pending_zero = true;
        continue;
      }
      if (character != '0') {
        oled_loop_pending_zero = false;
      } else {
        oled_loop_finish_number();
      }
      oled_loop_number_started = true;
    } else {
      oled_loop_finish_number();
    }

    oled_loop_append_char(character);
  }

  if (!oled_loop_pending_shift) {
    oled_loop_finish_number();
    oled_loop_compact_line();
  }
  oled_loop_preview[21] = '\0';
  oled_loop_preview_active = true;
  oled_render_frame();
}

void hardware_init_battery_adc(void) {
  // Descubre el canal ADC asociado al pin, configura su atenuacion y prepara
  // la calibracion por ajuste de curva cuando el chip y el SDK la soportan.
  adc_unit_t unit;
  ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BAT_VOLTAGE_ADC_PIN, &unit, &battery_adc_channel));

  const adc_oneshot_unit_init_cfg_t unit_config = {
      .unit_id = unit,
      .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
      .ulp_mode = ADC_ULP_MODE_DISABLE,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &battery_adc_handle));

  const adc_oneshot_chan_cfg_t channel_config = {
      .atten = ADC_ATTEN_DB_12,
      .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(battery_adc_handle, battery_adc_channel, &channel_config));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  const adc_cali_curve_fitting_config_t cali_config = {
      .unit_id = unit,
      .chan = battery_adc_channel,
      .atten = channel_config.atten,
      .bitwidth = channel_config.bitwidth,
  };
  if (adc_cali_create_scheme_curve_fitting(&cali_config, &battery_adc_cali_handle) != ESP_OK) {
    ESP_LOGW(TAG, "Calibracion ADC no disponible; se usara una conversion aproximada");
  }
#endif

  ESP_LOGI(TAG, "ADC de bateria inicializado en GPIO%d", BAT_VOLTAGE_ADC_PIN);
}

int hardware_read_battery_voltage_mv(void) {
  // Lee la bateria y devuelve milivoltios corregidos por el divisor resistivo.
  // Prefiere el resultado calibrado; si la calibracion no existe usa la
  // conversion aproximada del ADC de 12 bits. Devuelve -1 ante un fallo.
  if (battery_adc_handle == NULL) {
    return -1;
  }

  int voltage_mv = 0;
  if (battery_adc_cali_handle != NULL &&
      adc_oneshot_get_calibrated_result(battery_adc_handle, battery_adc_cali_handle, battery_adc_channel, &voltage_mv) == ESP_OK) {
    return (int)(voltage_mv * BAT_VOLTAGE_DIVIDER_RATIO);
  }

  int raw = 0;
  if (adc_oneshot_read(battery_adc_handle, battery_adc_channel, &raw) != ESP_OK) {
    return -1;
  }
  return (int)((raw * 3300.0f / 4095.0f) * BAT_VOLTAGE_DIVIDER_RATIO);
}

// Configura el puerto UART para la comunicación con la celda de carga.
void hardware_init_uart(void) {
  // Configura UART1 con el formato y la velocidad del protocolo de la celda,
  // asigna los pines definidos en hardware.h, reserva sus buffers y descarta
  // bytes residuales que pudieran confundirse con una cabecera valida.
  uart_config_t uart_config = {
      .baud_rate = BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
  ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));

  // Limpia cualquier byte residual de UART para evitar ruidos en el arranque.
  ESP_ERROR_CHECK(uart_flush_input(UART_PORT_NUM));
  ESP_ERROR_CHECK(uart_flush(UART_PORT_NUM));

  ESP_LOGI(TAG, "UART1 inicializada en GPIO18 (RX) y GPIO17 (TX)");
}

static esp_err_t init_storage(void) {
  // Localiza la particion FAT `archivos`, monta wear leveling e instala TinyUSB
  // MSC con automontaje desactivado. El volumen queda inicialmente montado
  // para la aplicacion y puede transferirse despues al host USB sin desmontar
  // ni volver a inicializar la memoria flash.
  ESP_LOGI(TAG, "Inicializando almacenamiento FAT");

  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "archivos");
  if (partition == NULL) {
    ESP_LOGE(TAG, "No se encontro la particion FAT archivos");
    return ESP_ERR_NOT_FOUND;
  }

  ESP_RETURN_ON_ERROR(wl_mount(partition, &msc_wl_handle), TAG,
                      "No se pudo montar FAT con wear leveling");

    tinyusb_msc_driver_config_t msc_driver_config = {
      .user_flags.auto_mount_off = 1,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&msc_driver_config), TAG,
              "No se pudo instalar el controlador MSC");

    tinyusb_msc_storage_config_t storage_config = {
      .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
      .fat_fs = {
          .base_path = "/archivos",
          .config.max_files = 10,
          .do_not_format = true,
          .format_flags = FM_ANY,
      },
      .medium.wl_handle = msc_wl_handle,
  };
  ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_spiflash(
                          &storage_config, &msc_storage_handle),
                      TAG, "No se pudo crear el almacenamiento MSC");

#if CONFIG_FATFS_USE_LABEL
  f_setlabel("ASIGNA_DL");
#endif

  ESP_LOGI(TAG, "Modo logger activo; FAT montado en /archivos");
  return ESP_OK;
}

// Informa si TinyUSB tiene actualmente la particion FAT entregada al host.
// La tarea principal usa este estado para evitar accesos de la aplicacion al
// volumen mientras Windows lo presenta como pendrive.
bool hardware_usb_msc_active(void) { return usb_msc_active; }

// Informa si TinyUSB notifico la desconexion fisica del host USB.
// La bandera permanece activa hasta que se inicia otra sesion USB, permitiendo
// que una tarea bloqueada en espera detecte el evento sin depender del callback.
bool hardware_usb_msc_detached(void) { return usb_msc_detached; }

esp_err_t hardware_enter_usb_msc(void) {
  // Cambia el punto de montaje de la FAT desde la aplicacion al dispositivo
  // USB e instala TinyUSB. Si alguno de los pasos falla, conserva el volumen
  // bajo control de la aplicacion y devuelve el error al llamador.
  if (usb_msc_active) {
    return ESP_OK;
  }
  if (msc_storage_handle == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  usb_msc_detached = false;

  esp_err_t error = tinyusb_msc_set_storage_mount_point(
      msc_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "No se pudo ceder FAT al host USB: %s", esp_err_to_name(error));
    return error;
  }

  tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
  usb_config.event_cb = usb_event_callback;
  error = tinyusb_driver_install(&usb_config);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "No se pudo iniciar el dispositivo USB: %s", esp_err_to_name(error));
    return error;
  }

  usb_msc_active = true;
  ESP_LOGI(TAG, "Pendrive USB listo; la aplicacion no accedera al volumen FAT");
  return ESP_OK;
}

esp_err_t hardware_exit_usb_msc(void) {
  // Devuelve la FAT al punto de montaje de la aplicacion y desinstala TinyUSB.
  // Solo marca MSC como inactivo despues de completar ambas operaciones para
  // que el resto del firmware no crea que el volumen ya es utilizable antes
  // de tiempo.
  if (!usb_msc_active) {
    return ESP_OK;
  }

  esp_err_t error = tinyusb_msc_set_storage_mount_point(
      msc_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_APP);
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "No se pudo recuperar la FAT para la aplicacion: %s", esp_err_to_name(error));
    return error;
  }

  error = tinyusb_driver_uninstall();
  if (error != ESP_OK) {
    ESP_LOGE(TAG, "No se pudo detener el dispositivo USB: %s", esp_err_to_name(error));
    return error;
  }

  usb_msc_active = false;
  ESP_LOGI(TAG, "FAT disponible para la aplicacion");
  return ESP_OK;
}

// Inicializa toda la capa de hardware en el orden requerido por el firmware.
// Primero monta el almacenamiento, luego configura UART y ADC, y finalmente
// el OLED, que puede dibujar el estado inicial usando esos valores.
void hardware_init_all(void) {
  ESP_ERROR_CHECK(init_storage());

  hardware_init_uart();
  hardware_init_battery_adc();
  hardware_init_oled();
}