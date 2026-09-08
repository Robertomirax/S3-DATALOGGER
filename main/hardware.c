//------------------------------ hardware.c --------------------------------
// Implementación de la inicialización del hardware del datalogger.
// Aquí se configura la capa UART, la salida del LED RGB y el montaje de LittleFS.

#include "hardware.h"
#include "driver/gpio.h" // IWYU pragma: keep
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#include "driver/uart.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stddef.h>

#include <stdint.h>
#include <stdlib.h>

static const char *TAG = "HARDWARE";

static rmt_channel_handle_t ws2812_channel = NULL;
static rmt_encoder_handle_t ws2812_encoder = NULL;
static SemaphoreHandle_t ws2812_mutex = NULL;
static uint8_t ws2812_red = 0;
static uint8_t ws2812_green = 0;
static uint8_t ws2812_blue = 0;
static adc_oneshot_unit_handle_t battery_adc_handle = NULL;
static adc_cali_handle_t battery_adc_cali_handle = NULL;
static adc_channel_t battery_adc_channel;

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

// Inicializa el LED RGB WS2812B usando el periférico RMT.
static void hardware_init_ws2812(void) {
  ws2812_mutex = xSemaphoreCreateMutex();
  if (ws2812_mutex == NULL) {
    ESP_LOGE(TAG, "No se pudo crear el mutex del WS2812");
    return;
  }

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

// Envía un color RGB al LED WS2812B usando el protocolo de datos RMT.
void hardware_ws2812_set_color(uint8_t red, uint8_t green, uint8_t blue) {
  if (ws2812_channel == NULL || ws2812_encoder == NULL) {
    ESP_LOGW(TAG, "WS2812 no esta inicializado");
    return;
  }

  if (xSemaphoreTake(ws2812_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  uint8_t grb[3] = {green, red, blue};
  const rmt_transmit_config_t transmit_config = {.loop_count = 0};
  ESP_ERROR_CHECK(rmt_transmit(ws2812_channel, ws2812_encoder, grb, sizeof(grb), &transmit_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(ws2812_channel, -1));
  esp_rom_delay_us(80);
  ws2812_red = red;
  ws2812_green = green;
  ws2812_blue = blue;
  xSemaphoreGive(ws2812_mutex);
}

void hardware_ws2812_flash_color(uint8_t red, uint8_t green, uint8_t blue, uint32_t duration_ms) {
  if (ws2812_channel == NULL || ws2812_encoder == NULL || ws2812_mutex == NULL) {
    ESP_LOGW(TAG, "WS2812 no esta inicializado");
    return;
  }

  if (xSemaphoreTake(ws2812_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  uint8_t previous_red = ws2812_red;
  uint8_t previous_green = ws2812_green;
  uint8_t previous_blue = ws2812_blue;
  uint8_t grb[3] = {green, red, blue};
  const rmt_transmit_config_t transmit_config = {.loop_count = 0};

  ESP_ERROR_CHECK(rmt_transmit(ws2812_channel, ws2812_encoder, grb, sizeof(grb), &transmit_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(ws2812_channel, -1));
  vTaskDelay(pdMS_TO_TICKS(duration_ms));

  grb[0] = previous_green;
  grb[1] = previous_red;
  grb[2] = previous_blue;
  ESP_ERROR_CHECK(rmt_transmit(ws2812_channel, ws2812_encoder, grb, sizeof(grb), &transmit_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(ws2812_channel, -1));
  esp_rom_delay_us(80);
  xSemaphoreGive(ws2812_mutex);
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

// Monta la partición LittleFS usada para guardar el log del datalogger.
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

// Inicializa toda la capa de hardware del sistema en el orden correcto.
void hardware_init_all(void) {
  ESP_ERROR_CHECK(init_littlefs());

  hardware_init_ws2812();
  hardware_init_uart();
  hardware_init_battery_adc();
}