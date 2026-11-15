#pragma once
#include "esp_err.h"
#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO 0
typedef void *spi_device_handle_t;
typedef struct { size_t length; const void *tx_buffer; } spi_transaction_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; } spi_device_interface_config_t;
esp_err_t spi_device_transmit(spi_device_handle_t, spi_transaction_t *);
static inline esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *tx)
{ (void)device; (void)tx; return ESP_OK; }
static inline esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma)
{ (void)host; (void)cfg; (void)dma; return ESP_OK; }
static inline esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *cfg, spi_device_handle_t *out)
{ (void)host; (void)cfg; *out = (void *)1; return ESP_OK; }
void *spi_bus_dma_memory_alloc(int, size_t, unsigned);
