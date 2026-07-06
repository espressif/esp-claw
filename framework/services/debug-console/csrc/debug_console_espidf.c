/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_err.h"

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_console.h"
#include "soc/soc_caps.h"
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#if CONFIG_ESP_CONSOLE_USB_CDC
#include "esp_vfs_cdcacm.h"
#endif

static bool s_debug_console_espidf_initialized;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
static esp_err_t init_uart_console(void)
{
    esp_console_dev_uart_config_t dev_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    fflush(stdout);
    fsync(fileno(stdout));

    uart_vfs_dev_port_set_rx_line_endings(dev_config.channel, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(dev_config.channel, ESP_LINE_ENDINGS_CRLF);

#if SOC_UART_SUPPORT_REF_TICK
    uart_sclk_t clk_source = UART_SCLK_REF_TICK;
    if (dev_config.baud_rate > 1000000) {
        clk_source = UART_SCLK_DEFAULT;
    }
#elif SOC_UART_SUPPORT_XTAL_CLK
    uart_sclk_t clk_source = UART_SCLK_XTAL;
#else
#error "No UART clock source is aware of DFS"
#endif

    const uart_config_t uart_config = {
        .baud_rate = dev_config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .source_clk = clk_source,
    };

    esp_err_t err = uart_param_config(dev_config.channel, &uart_config);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_set_pin(dev_config.channel,
                       dev_config.tx_gpio_num,
                       dev_config.rx_gpio_num,
                       -1,
                       -1);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_driver_install(dev_config.channel, 256, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    uart_vfs_dev_use_driver(dev_config.channel);
    return ESP_OK;
}
#endif

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static esp_err_t init_usb_serial_jtag_console(void)
{
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);

    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
}
#endif

#if CONFIG_ESP_CONSOLE_USB_CDC
static esp_err_t init_usb_cdc_console(void)
{
    esp_vfs_dev_cdcacm_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    esp_vfs_dev_cdcacm_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin), F_SETFL, 0);
    return ESP_OK;
}
#endif

int claw_debug_console_espidf_init(void)
{
    if (s_debug_console_espidf_initialized) {
        return ESP_OK;
    }

    esp_err_t err;
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    err = init_uart_console();
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    err = init_usb_serial_jtag_console();
#elif CONFIG_ESP_CONSOLE_USB_CDC
    err = init_usb_cdc_console();
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif

    if (err == ESP_OK) {
        s_debug_console_espidf_initialized = true;
    }

    return err;
}

int claw_debug_console_espidf_read(uint8_t *bytes, size_t len, size_t *out_read)
{
    if (!bytes || !out_read) {
        return ESP_ERR_INVALID_ARG;
    }

    ssize_t read_len = read(STDIN_FILENO, bytes, len);
    if (read_len < 0) {
        return ESP_FAIL;
    }

    *out_read = (size_t)read_len;
    return ESP_OK;
}

int claw_debug_console_espidf_write(const uint8_t *bytes, size_t len, size_t *out_written)
{
    if (!bytes || !out_written) {
        return ESP_ERR_INVALID_ARG;
    }

    ssize_t written = write(STDOUT_FILENO, bytes, len);
    if (written < 0) {
        return ESP_FAIL;
    }

    *out_written = (size_t)written;
    return ESP_OK;
}

int claw_debug_console_espidf_flush(void)
{
    fflush(stdout);
    fsync(STDOUT_FILENO);
    return ESP_OK;
}
