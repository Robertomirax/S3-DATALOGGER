//------------------------------ hardware.c --------------------------------

#include "hardware.h"
#include "driver/gpio.h" // IWYU pragma: keep
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#include "driver/uart.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include <stddef.h>
#include <inttypes.h>
//#include "esp_rom_sys.h"
#include "hal/gpio_types.h"
#include <stdlib.h>
#include <stdint.h>
#include "esp_lcd_panel_vendor.h"  // IWYU pragma: keep
#include "esp_lcd_panel_ops.h"


static const char *TAG = "HARDWARE";



lv_disp_t *lvgl_disp = NULL;
static rmt_channel_handle_t ws2812_channel = NULL;
static rmt_encoder_handle_t ws2812_encoder = NULL;




void hardware_init_gpio(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BLINK_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);
  gpio_set_level(BLINK_GPIO, 1);

  gpio_config_t bk_gpio_config = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = (1ULL << PIN_NUM_BK_LIGHT),
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&bk_gpio_config);
  gpio_set_level(PIN_NUM_BK_LIGHT, 0); // Apaga la luz de fondo al inicio
}

static void hardware_init_ws2812(void) {
  const rmt_tx_channel_config_t channel_config = {
      .gpio_num = WS2812_GPIO,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
      .mem_block_symbols = 64,
      .trans_queue_depth = 1,
  };
  ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &ws2812_channel));

  const rmt_bytes_encoder_config_t encoder_config = {
      .bit0 = {
          .duration0 = 3,
          .level0 = 1,
          .duration1 = 9,
          .level1 = 0,
      },
      .bit1 = {
          .duration0 = 6,
          .level0 = 1,
          .duration1 = 6,
          .level1 = 0,
      },
      .flags = {.msb_first = 1},
  };
  ESP_ERROR_CHECK(rmt_new_bytes_encoder(&encoder_config, &ws2812_encoder));
  ESP_ERROR_CHECK(rmt_enable(ws2812_channel));

  hardware_ws2812_set_color(255, 0, 0);
  ESP_LOGI(TAG, "WS2812 inicializado en GPIO%d", WS2812_GPIO);
}

void hardware_ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue) {
  if (ws2812_channel == NULL || ws2812_encoder == NULL) {
    ESP_LOGW(TAG, "WS2812 no esta inicializado");
    return;
  }

  uint8_t grb[3] = {green, red, blue};
  const rmt_transmit_config_t transmit_config = {.loop_count = 0};
  ESP_ERROR_CHECK(rmt_transmit(ws2812_channel, ws2812_encoder, grb, sizeof(grb), &transmit_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(ws2812_channel, -1));
  esp_rom_delay_us(80);
}

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

  // 2. Limpiar rigurosamente cualquier byte residual en hardware/software
  ESP_ERROR_CHECK(uart_flush_input(UART_PORT_NUM));
  ESP_ERROR_CHECK(uart_flush(UART_PORT_NUM));

  ESP_LOGI(TAG, "UART1 inicializada en GPIO18 (RX) y GPIO17 (TX)");
}
lv_disp_t *hardware_init_display(void) {
  ESP_LOGI(TAG, "Iniciando Bus SPI...");
  spi_bus_config_t buscfg = {
      .sclk_io_num = PIN_NUM_SCLK,
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
  };
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Configurando Panel ST7789...");
  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = PIN_NUM_LCD_DC,
      .cs_gpio_num = PIN_NUM_LCD_CS,
      .pclk_hz = 20 * 1000 * 1000,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .spi_mode = 3,
      .trans_queue_depth = 10,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((spi_host_device_t)LCD_HOST, &io_config, &io_handle));

  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = PIN_NUM_LCD_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
      .bits_per_pixel = 16,
  };

  // Uso del driver dedicado para ST7789 en ESP-IDF v6
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  ESP_LOGI(TAG, "Inicializando LVGL Port...");
  // Imprimir reporte de memoria libre
  ESP_LOGI("MEM_CHECK", "SRAM Interna libre: %" PRIu32 " KB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
  ESP_LOGI("MEM_CHECK", "PSRAM Octal libre:  %" PRIu32 " KB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

  const lvgl_port_display_cfg_t disp_cfg = {.io_handle = io_handle,
                                            .panel_handle = panel_handle,
                                            .buffer_size = LCD_H_RES * 20,
                                            .double_buffer = true,
                                            .hres = LCD_H_RES,
                                            .vres = LCD_V_RES,
                                            .monochrome = false,
                                            .rotation =
                                                {
                                                    .swap_xy = false,
                                                    .mirror_x = false,
                                                    .mirror_y = false,
                                                },
                                            .flags = {
                                                .buff_dma = true,
                                            }};

  lvgl_disp = lvgl_port_add_disp(&disp_cfg);
  return lvgl_disp;
}

// Configurar y montar LittleFS
static esp_err_t init_littlefs(void) {
  ESP_LOGI(TAG, "Inicializando LittleFS");

  esp_vfs_littlefs_conf_t conf = {.base_path = "/archivos", .partition_label = "archivos", .format_if_mount_failed = false, .dont_mount = false};

  esp_err_t ret = esp_vfs_littlefs_register(&conf);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Fallo al montar o formatear LittleFS");
    } else {
      ESP_LOGE(TAG, "Error al inicializar LittleFS (%s)", esp_err_to_name(ret));
    }
    return ret;
  }

  size_t total = 0, used = 0;
  ret = esp_littlefs_info(conf.partition_label, &total, &used);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "LittleFS Montado. Total: %d, Usado: %d", total, used);
  }
  return ESP_OK;
}

void hardware_init_all(void) {
  ESP_ERROR_CHECK(init_littlefs());
  hardware_init_gpio();
  hardware_init_ws2812();
  hardware_init_uart();
  hardware_init_display();
 
}