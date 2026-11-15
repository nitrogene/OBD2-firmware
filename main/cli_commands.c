#include "cli_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cli";

#define KLINE_UART_PORT  UART_NUM_1
#define KLINE_BAUD_RATE  10400

static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    (void)esp_flash_get_size(NULL, &flash_size);
    size_t psram_size = esp_psram_get_size();

    printf("OBD2-firmware MODE_BRINGUP\n");
    printf("  IDF Version : %s\n", esp_get_idf_version());
    printf("  Chip        : ESP32-S3 rev v%d.%d (%d cores)\n",
           chip.revision / 100, chip.revision % 100, chip.cores);
    printf("  Flash       : %lu MB\n", (unsigned long)(flash_size / (1024UL * 1024UL)));
    printf("  PSRAM       : %lu MB\n", (unsigned long)(psram_size / (1024UL * 1024UL)));
    printf("  Pins        : VBAT=IO%d LED=IO%d KLINE_RX=IO%d KLINE_TX=IO%d TWAI_TX=IO%d TWAI_RX=IO%d\n",
           (int)BSP_PIN_VBAT_SENSE, (int)BSP_PIN_LED_STATUS,
           (int)BSP_PIN_KLINE_RX, (int)BSP_PIN_KLINE_TX,
           (int)BSP_PIN_TWAI_TX, (int)BSP_PIN_TWAI_RX);
    return 0;
}

static int cmd_led(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: led <off|breath|blink|solid|burst|sos|duty <0-100>>\n");
        return 1;
    }

    const char *mode = argv[1];
    if (strcmp(mode, "off") == 0) {
        bsp_led_set_pattern(BSP_LED_OFF);
    } else if (strcmp(mode, "breath") == 0) {
        bsp_led_set_pattern(BSP_LED_BREATHING);
    } else if (strcmp(mode, "blink") == 0) {
        bsp_led_set_pattern(BSP_LED_BLINK_SLOW);
    } else if (strcmp(mode, "solid") == 0) {
        bsp_led_set_pattern(BSP_LED_SOLID);
    } else if (strcmp(mode, "burst") == 0) {
        bsp_led_set_pattern(BSP_LED_BURST);
    } else if (strcmp(mode, "sos") == 0) {
        bsp_led_set_pattern(BSP_LED_SOS);
    } else if (strcmp(mode, "duty") == 0) {
        if (argc < 3) {
            printf("Usage: led duty <0-100>\n");
            return 1;
        }
        long val = strtol(argv[2], NULL, 10);
        if (val < 0 || val > 100) {
            printf("Error: duty must be 0..100\n");
            return 1;
        }
        bsp_led_set_duty((uint8_t)val);
    } else {
        printf("Unknown LED mode '%s'\n", mode);
        return 1;
    }

    printf("LED1 -> %s\n", mode);
    return 0;
}

static int cmd_vbat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t adc_mv = 0;
    uint32_t vbat_mv = 0;
    esp_err_t err = bsp_vbat_read_mv(&adc_mv, &vbat_mv);
    if (err != ESP_OK) {
        printf("VBAT read failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("V_ADC = %lu mV | V_BAT = %lu mV (%.2f V)\n",
           (unsigned long)adc_mv,
           (unsigned long)vbat_mv,
           (double)vbat_mv / 1000.0);
    return 0;
}

static int cmd_can(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: can <ping|pong|rx> [timeout_ms]\n");
        return 1;
    }

    const char *sub = argv[1];
    uint32_t timeout_ms = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 2000U;

    const twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(
        BSP_PIN_TWAI_TX, BSP_PIN_TWAI_RX, TWAI_MODE_NORMAL);
    const twai_timing_config_t t_cfg = TWAI_TIMING_CONFIG_500KBITS();
    const twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_cfg, &t_cfg, &f_cfg);
    if (err != ESP_OK) {
        printf("TWAI install failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    err = twai_start();
    if (err != ESP_OK) {
        printf("TWAI start failed: %s\n", esp_err_to_name(err));
        (void)twai_driver_uninstall();
        return 1;
    }

    int rc = 0;
    if (strcmp(sub, "ping") == 0) {
        twai_message_t tx = {
            .identifier = 0x7DF,
            .data_length_code = 8,
            .data = {0x02, 0x01, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55},
        };
        err = twai_transmit(&tx, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            printf("CAN TX 0x7DF failed: %s\n", esp_err_to_name(err));
            rc = 1;
        } else {
            printf("CAN TX 0x7DF OK, waiting %lu ms for pong...\n", (unsigned long)timeout_ms);
            twai_message_t rx = {0};
            err = twai_receive(&rx, pdMS_TO_TICKS(timeout_ms));
            if (err == ESP_OK) {
                printf("CAN RX ID=0x%03lX DLC=%d data=%02X %02X %02X %02X\n",
                       (unsigned long)rx.identifier, rx.data_length_code,
                       rx.data[0], rx.data[1], rx.data[2], rx.data[3]);
            } else {
                printf("CAN RX timeout/error: %s\n", esp_err_to_name(err));
                rc = 1;
            }
        }
    } else if (strcmp(sub, "pong") == 0) {
        printf("CAN listening for ping (%lu ms)...\n", (unsigned long)timeout_ms);
        twai_message_t rx = {0};
        err = twai_receive(&rx, pdMS_TO_TICKS(timeout_ms));
        if (err == ESP_OK) {
            printf("CAN RX ID=0x%03lX, replying 0x7E8...\n", (unsigned long)rx.identifier);
            twai_message_t tx = {
                .identifier = 0x7E8,
                .data_length_code = 8,
                .data = {0x06, 0x41, 0x00, 0xBF, 0x9F, 0xA8, 0x93, 0xAA},
            };
            err = twai_transmit(&tx, pdMS_TO_TICKS(500));
            printf("CAN TX 0x7E8: %s\n", esp_err_to_name(err));
            if (err != ESP_OK) {
                rc = 1;
            }
        } else {
            printf("CAN RX timeout/error: %s\n", esp_err_to_name(err));
            rc = 1;
        }
    } else if (strcmp(sub, "rx") == 0) {
        printf("CAN RX listen (%lu ms)...\n", (unsigned long)timeout_ms);
        twai_message_t rx = {0};
        err = twai_receive(&rx, pdMS_TO_TICKS(timeout_ms));
        if (err == ESP_OK) {
            printf("CAN RX ID=0x%03lX ext=%d DLC=%d\n",
                   (unsigned long)rx.identifier, rx.extd, rx.data_length_code);
        } else {
            printf("CAN RX: %s\n", esp_err_to_name(err));
            rc = 1;
        }
    } else {
        printf("Unknown subcommand '%s'\n", sub);
        rc = 1;
    }

    (void)twai_stop();
    (void)twai_driver_uninstall();
    return rc;
}

static int cmd_kline(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: kline <echo|pulse>\n");
        return 1;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "echo") == 0) {
        const uart_config_t uart_cfg = {
            .baud_rate = KLINE_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t err = uart_driver_install(KLINE_UART_PORT, 256, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            printf("UART install failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        (void)uart_param_config(KLINE_UART_PORT, &uart_cfg);
        (void)uart_set_pin(KLINE_UART_PORT, BSP_PIN_KLINE_TX, BSP_PIN_KLINE_RX,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        (void)uart_flush_input(KLINE_UART_PORT);

        const uint8_t tx_bytes[4] = {0x81, 0x10, 0xF1, 0x81};
        uint8_t rx_bytes[4] = {0};
        int written = uart_write_bytes(KLINE_UART_PORT, tx_bytes, sizeof(tx_bytes));
        (void)uart_wait_tx_done(KLINE_UART_PORT, pdMS_TO_TICKS(100));
        int read_len = uart_read_bytes(KLINE_UART_PORT, rx_bytes, sizeof(rx_bytes), pdMS_TO_TICKS(100));

        printf("K-Line TX %d bytes, RX echo %d bytes:", written, read_len);
        for (int i = 0; i < read_len; ++i) {
            printf(" %02X", rx_bytes[i]);
        }
        bool match = (read_len == (int)sizeof(tx_bytes)) && (memcmp(tx_bytes, rx_bytes, sizeof(tx_bytes)) == 0);
        printf(" -> %s\n", match ? "ECHO OK" : "ECHO MISMATCH/NO TRANSCEIVER");

        (void)uart_driver_delete(KLINE_UART_PORT);
        return match ? 0 : 1;
    }

    if (strcmp(sub, "pulse") == 0) {
        /* Fast-Init wakeup pulse: 25 ms dominant (LOW on IO5) + 25 ms recessive (HIGH on IO5) */
        const gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << BSP_PIN_KLINE_TX),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&io_cfg);
        (void)gpio_set_level(BSP_PIN_KLINE_TX, 1);
        vTaskDelay(pdMS_TO_TICKS(10));

        (void)gpio_set_level(BSP_PIN_KLINE_TX, 0);
        esp_rom_delay_us(25000);
        (void)gpio_set_level(BSP_PIN_KLINE_TX, 1);
        esp_rom_delay_us(25000);

        printf("K-Line 25 ms dominant / 25 ms recessive Fast-Init pulse sent on IO%d\n",
               (int)BSP_PIN_KLINE_TX);
        return 0;
    }

    printf("Unknown kline subcommand '%s'\n", sub);
    return 1;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "Print chip, memory, and BSP GPIO info",
            .hint = NULL,
            .func = &cmd_info,
        },
        {
            .command = "led",
            .help = "Control LED1 (IO2): led <off|breath|blink|solid|burst|sos|duty <0-100>>",
            .hint = NULL,
            .func = &cmd_led,
        },
        {
            .command = "vbat",
            .help = "Sample VBAT_SENSE (IO1 / ADC1_CH0)",
            .hint = NULL,
            .func = &cmd_vbat,
        },
        {
            .command = "can",
            .help = "500 kbps TWAI bringup: can <ping|pong|rx> [timeout_ms]",
            .hint = NULL,
            .func = &cmd_can,
        },
        {
            .command = "kline",
            .help = "10,400 baud K-Line bringup: kline <echo|pulse>",
            .hint = NULL,
            .func = &cmd_kline,
        },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

esp_err_t cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "obd2> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG REPL init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_console_register_help_command());
    register_commands();

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start REPL: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB-C CDC MODE_BRINGUP CLI ready (type 'help')");
    return ESP_OK;
}
