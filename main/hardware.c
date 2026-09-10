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
static char oled_uart_preview[44];
static size_t oled_uart_preview_len = 0;
static bool oled_uart_preview_active = false;
static char oled_loop_preview[44];
static size_t oled_loop_preview_len = 0;
static bool oled_loop_number_started = false;
static bool oled_loop_pending_zero = false;
static bool oled_loop_preview_active = false;
static bool oled_status_active = false;
static bool usb_msc_active = false;
static volatile bool usb_msc_detached = false;

static tinyusb_msc_storage_handle_t msc_storage_handle;
static wl_handle_t msc_wl_handle = WL_INVALID_HANDLE;

static void usb_event_callback(tinyusb_event_t *event, void *arg) {
  (void)arg;
  if (event != NULL && event->id == TINYUSB_EVENT_DETACHED) {
    usb_msc_detached = true;
  }
}

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

static void oled_draw_text(const char *text, uint8_t column, uint8_t page) {
  while (*text != '\0' && column < 123) {
    const uint8_t *glyph = oled_glyph(*text++);
    for (uint8_t index = 0; index < 5 && column + index < 128; index++) {
      oled_framebuffer[(page * 128) + column + index] = glyph[index];
    }
    column += 6;
  }
}

static void oled_draw_wrapped_text(const char *text, uint8_t page) {
  uint8_t column = 0;
  while (*text != '\0' && page < 8) {
    if (*text == '\r') {
      text++;
      continue;
    }
    if (*text == '\n') {
      text++;
      column = 0;
      page++;
      continue;
    }

    const uint8_t *glyph = oled_glyph(*text++);
    for (uint8_t index = 0; index < 5 && column + index < 128; index++) {
      oled_framebuffer[(page * 128) + column + index] = glyph[index];
    }
    column += 6;
    if (column >= 126) {
      column = 0;
      page++;
    }
  }
}

static void oled_draw_battery_line(void) {
  char battery_text[22];
  if (oled_battery_voltage_mv >= 0) {
    snprintf(battery_text, sizeof(battery_text), "BAT: %d.%02d V", oled_battery_voltage_mv / 1000,
             (oled_battery_voltage_mv % 1000) / 10);
  } else {
    snprintf(battery_text, sizeof(battery_text), "BAT: -- V");
  }
  oled_draw_text(battery_text, 0, 0);

  if (oled_tara_valid) {
    char tara_text[22];
    snprintf(tara_text, sizeof(tara_text), "TARA = %.0f %%", oled_tara_percent);
    oled_draw_text(tara_text, 0, 2);
  }
}

static void oled_draw_firmware_line(void) {
  char firmware_text[16];
  int firmware_length = snprintf(firmware_text, sizeof(firmware_text), "FIRM %d", FIRM);
  if (firmware_length <= 0) {
    return;
  }

  int firmware_width = firmware_length * 6;
  uint8_t column = firmware_width < 128 ? (uint8_t)(128 - firmware_width) : 0;
  oled_draw_text(firmware_text, column, 0);
}

static void oled_loop_append_char(char character) {
  if (oled_loop_preview_len < 21) {
    oled_loop_preview[22 + oled_loop_preview_len++] = character;
  }
}

static void oled_loop_finish_number(void) {
  if (oled_loop_pending_zero) {
    oled_loop_append_char('0');
    oled_loop_pending_zero = false;
  }
  oled_loop_number_started = false;
}

static void oled_loop_compact_line(void) {
  char compact_line[22] = {0};
  size_t compact_len = 0;
  size_t number_count = 0;
  bool in_number = false;
  bool pending_space = false;

  for (size_t index = 22; index < 44 && oled_loop_preview[index] != '\0'; index++) {
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

  memcpy(oled_loop_preview + 22, compact_line, sizeof(compact_line));
  oled_loop_preview_len = compact_len;
}

static void oled_render_frame(void) {
  memset(oled_framebuffer, 0, sizeof(oled_framebuffer));
  if (oled_battery_visible) {
    oled_draw_battery_line();
  }
  oled_draw_firmware_line();
  if (oled_status_active) {
    oled_draw_text(oled_status_title, 0, 2);
    oled_draw_wrapped_text(oled_status_message, 4);
  } else if (!oled_loop_preview_active) {
    oled_draw_wrapped_text(oled_frame_text, 4);
  }
  if (oled_loop_preview_active) {
    oled_draw_text(oled_loop_preview, 0, 4);
    oled_draw_text(oled_loop_preview + 22, 0, 5);
  }
  if (oled_uart_preview_active) {
    oled_draw_text(oled_uart_preview, 0, 6);
    oled_draw_text(oled_uart_preview + 22, 0, 7);
  }
  ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(oled_panel, 0, 0, 128, 64, oled_framebuffer));
}

void hardware_init_oled(void) {
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
  if (oled_panel == NULL || !oled_battery_visible) {
    return;
  }
  oled_battery_voltage_mv = voltage_mv;
  oled_render_frame();
}

void hardware_oled_hide_battery(void) {
  oled_battery_visible = false;
  if (oled_panel != NULL) {
    oled_render_frame();
  }
}

void hardware_oled_show_message(const char *title, const char *message) {
  if (oled_panel == NULL) {
    return;
  }

  snprintf(oled_status_title, sizeof(oled_status_title), "%s", title != NULL ? title : "");
  snprintf(oled_status_message, sizeof(oled_status_message), "%s", message != NULL ? message : "");
  oled_status_active = true;
  oled_render_frame();
}

void hardware_oled_show_tara(float tara_percent) {
  if (oled_panel == NULL) {
    return;
  }

  oled_tara_percent = tara_percent;
  oled_tara_valid = true;
  oled_status_active = false;
  oled_render_frame();
}

void hardware_oled_show_frame(const char *frame) {
  if (oled_panel == NULL || frame == NULL) {
    return;
  }

  snprintf(oled_frame_text, sizeof(oled_frame_text), "%s", frame);
  oled_uart_preview_active = false;
  oled_status_active = false;
  oled_render_frame();
}

void hardware_oled_update_uart_preview(const uint8_t *data, size_t len) {
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

void hardware_oled_clear_uart_preview(void) {
  if (oled_panel == NULL) {
    return;
  }

  oled_uart_preview_active = false;
  oled_uart_preview_len = 0;
  oled_uart_preview[0] = '\0';
  oled_loop_preview_active = false;
  oled_loop_preview_len = 0;
  oled_loop_preview[0] = '\0';
  oled_render_frame();
}

void hardware_oled_update_loop_preview(const uint8_t *data, size_t len) {
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
      memcpy(oled_loop_preview, oled_loop_preview + 22, 22);
      memset(oled_loop_preview + 22, 0, 22);
      oled_loop_preview_len = 0;
      continue;
    }
    if ((unsigned char)character < 0x20 || (unsigned char)character > 0x7E) {
      continue;
    }

    if (character == ' ') {
      oled_loop_finish_number();
      if (oled_loop_preview_len == 0 || oled_loop_preview[22 + oled_loop_preview_len - 1] == ' ') {
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

  oled_loop_finish_number();
  oled_loop_compact_line();
  oled_loop_preview[21] = '\0';
  oled_loop_preview_active = true;
  oled_render_frame();
}

void hardware_init_battery_adc(void) {
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

  ESP_LOGI(TAG, "Modo logger activo; FAT montado en /archivos");
  return ESP_OK;
}

bool hardware_usb_msc_active(void) { return usb_msc_active; }

bool hardware_usb_msc_detached(void) { return usb_msc_detached; }

esp_err_t hardware_enter_usb_msc(void) {
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

// Inicializa toda la capa de hardware del sistema en el orden correcto.
void hardware_init_all(void) {
  ESP_ERROR_CHECK(init_storage());

  hardware_init_uart();
  hardware_init_battery_adc();
  hardware_init_oled();
}