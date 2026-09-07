//------------------------------ hardware.c --------------------------------

#include "hardware.h"
#include "driver/gpio.h" // IWYU pragma: keep
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#include "driver/uart.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include <stddef.h>

#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "HARDWARE";

static rmt_channel_handle_t ws2812_channel = NULL;
static rmt_encoder_handle_t ws2812_encoder = NULL;



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
      .bit0 =
          {
              .duration0 = 3,
              .level0 = 1,
              .duration1 = 9,
              .level1 = 0,
          },
      .bit1 =
          {
              .duration0 = 6,
              .level0 = 1,
              .duration1 = 6,
              .level1 = 0,
          },
      .flags = {.msb_first = 1},
  };
  ESP_ERROR_CHECK(rmt_new_bytes_encoder(&encoder_config, &ws2812_encoder));
  ESP_ERROR_CHECK(rmt_enable(ws2812_channel));

  hardware_ws2812_set_color(255, 0, 0); // Rojo al inicio
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
  
  hardware_init_ws2812();
  hardware_init_uart();
 
}