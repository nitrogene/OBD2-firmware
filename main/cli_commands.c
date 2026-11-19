#include "cli_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp.h"
#include "ecu_simulator.h"
#include "isotp.h"
#include "kline_driver.h"
#include "kwp2000.h"
#include "obd2_autodetect.h"
#include "power_mgmt.h"
#include "transport_stream.h"
#include "twai_driver.h"
#include "driver/gpio.h"
#include "driver/twai.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
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

#define KLINE_UART_PORT UART_NUM_1
#define KLINE_BAUD_RATE 10400

static bool parse_long_arg(const char *str, long *out_val) {
    if (str == NULL || *str == '\0' || out_val == NULL) {
        return false;
    }
    char *endptr = NULL;
    long v = strtol(str, &endptr, 10);
    if (endptr == str || *endptr != '\0') {
        return false;
    }
    *out_val = v;
    return true;
}

static esp_err_t usb_cli_transport_tx(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    if (data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)fwrite(data, 1U, len, stdout);
    (void)fflush(stdout);
    return ESP_OK;
}

static int cmd_at(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: at <AT/OBD command> (e.g. at ATZ, at 010C, at AT+EXT=1)\n");
        return 1;
    }

    char line[128];
    size_t pos = 0U;
    for (int i = 1; i < argc && pos + 2U < sizeof(line); ++i) {
        int n =
            snprintf(&line[pos], sizeof(line) - pos - 1U, (i + 1 < argc) ? "%s " : "%s", argv[i]);
        if (n > 0) {
            pos += (size_t)n;
        }
    }
    line[pos++] = '\r';
    line[pos] = '\0';

    power_mgmt_notify_activity();
    esp_err_t err = transport_stream_feed_rx(TRANSPORT_ID_USB, (const uint8_t *)line, pos);
    if (err != ESP_OK) {
        printf("Transport busy or error: %s\n", esp_err_to_name(err));
        return 1;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    printf("\n");
    return 0;
}

static int cmd_info(int argc, char **argv) {
    (void)argc;
    (void)argv;

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    (void)esp_flash_get_size(NULL, &flash_size);
    size_t psram_size = esp_psram_get_size();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    printf("OBD2-firmware (%s)\n",
#ifdef CONFIG_OBD2_VIRTUAL_BENCH
           "VIRTUAL_BENCH"
#else
           "STANDARD"
#endif
    );
    printf("  App Version : %s (%s %s)\n", app_desc->version, app_desc->date, app_desc->time);
    printf("  IDF Version : %s\n", esp_get_idf_version());
    printf("  Chip        : ESP32-S3 rev v%d.%d (%d cores)\n", chip.revision / 100,
           chip.revision % 100, chip.cores);
    printf("  Flash       : %lu MB\n", (unsigned long)(flash_size / (1024UL * 1024UL)));
    printf("  PSRAM       : %lu MB\n", (unsigned long)(psram_size / (1024UL * 1024UL)));
    printf("  Pins        : VBAT=IO%d LED=IO%d KLINE_RX=IO%d KLINE_TX=IO%d TWAI_TX=IO%d "
           "TWAI_RX=IO%d\n",
           (int)BSP_PIN_VBAT_SENSE, (int)BSP_PIN_LED_STATUS, (int)BSP_PIN_KLINE_RX,
           (int)BSP_PIN_KLINE_TX, (int)BSP_PIN_TWAI_TX, (int)BSP_PIN_TWAI_RX);
    return 0;
}

static int cmd_led(int argc, char **argv) {
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
        long val = 0;
        if (!parse_long_arg(argv[2], &val) || val < 0 || val > 100) {
            printf("Error: duty must be an integer 0..100\n");
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

static int cmd_vbat(int argc, char **argv) {
    (void)argc;
    (void)argv;
    power_mgmt_notify_activity();

    uint32_t adc_mv = 0;
    uint32_t vbat_mv = 0;
    esp_err_t err = bsp_vbat_read_mv(&adc_mv, &vbat_mv);
    if (err != ESP_OK) {
        printf("VBAT read failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("V_ADC = %lu mV | V_BAT = %lu mV (%.2f V) [%s]\n", (unsigned long)adc_mv,
           (unsigned long)vbat_mv, (double)vbat_mv / 1000.0,
           power_mgmt_state_to_str(power_classify_vbat_mv(vbat_mv)));
    return 0;
}

static int cmd_power(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "sleep") == 0) {
        printf("Entering Deep Sleep now (wake on IO%d / IO%d LOW)...\n", (int)BSP_PIN_KLINE_RX,
               (int)BSP_PIN_TWAI_RX);
        power_mgmt_enter_deep_sleep();
    }
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        power_mgmt_notify_activity();
        esp_err_t err = power_mgmt_boot_self_test();
        printf("Boot self-test: %s\n", esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    power_mgmt_notify_activity();
    power_status_t st = {0};
    esp_err_t err = power_mgmt_get_status(&st);
    if (err != ESP_OK) {
        printf("power_mgmt_get_status failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Power Telemetry (32-sample moving avg):\n");
    printf("  State      : %s\n", power_mgmt_state_to_str(st.state));
    printf("  V_BAT      : %lu mV (%.2f V)\n", (unsigned long)st.vbat_mv,
           (double)st.vbat_mv / 1000.0);
    printf("  V_ADC      : %lu mV\n", (unsigned long)st.adc_mv);
    printf("  Auto-sleep : %s (idle %lu / %lu s)\n", st.auto_sleep_armed ? "ARMED" : "DISARMED",
           (unsigned long)(st.inactivity_ms / 1000U),
           (unsigned long)(POWER_INACTIVITY_TIMEOUT_MS / 1000U));
    printf("  Wake cause : %d\n", (int)st.wakeup_cause);
    return 0;
}

static int cmd_can(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: can <ping|pong|rx|probe> [timeout_ms]\n");
        return 1;
    }

    const char *sub = argv[1];
    uint32_t timeout_ms = 2000U;
    if (argc >= 3) {
        long tval = 0;
        if (!parse_long_arg(argv[2], &tval) || tval <= 0 || tval > 60000) {
            printf("Error: timeout_ms must be 1..60000\n");
            return 1;
        }
        timeout_ms = (uint32_t)tval;
    }
    power_mgmt_notify_activity();

    if (strcmp(sub, "probe") == 0) {
        twai_drv_bitrate_t detected = TWAI_DRV_BITRATE_500K;
        esp_err_t err = twai_drv_probe_bitrate(timeout_ms, &detected);
        if (err == ESP_OK) {
            printf("CAN listen-only probe: %d bps\n", (int)detected);
            return 0;
        }
        printf("CAN listen-only probe: no traffic (%s)\n", esp_err_to_name(err));
        return 1;
    }

    esp_err_t err = twai_drv_init(TWAI_DRV_BITRATE_500K, TWAI_DRV_MODE_NORMAL, NULL);
    if (err != ESP_OK) {
        printf("TWAI init failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    int rc = 0;
    if (strcmp(sub, "ping") == 0) {
        twai_drv_frame_t tx = {
            .id = 0x7DF,
            .dlc = 8,
            .data = {0x02, 0x01, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55},
        };
        err = twai_drv_send(&tx, 500);
        if (err != ESP_OK) {
            printf("CAN TX 0x7DF failed: %s\n", esp_err_to_name(err));
            rc = 1;
        } else {
            printf("CAN TX 0x7DF OK, waiting %lu ms for pong...\n", (unsigned long)timeout_ms);
            twai_drv_frame_t rx = {0};
            err = twai_drv_receive(&rx, timeout_ms);
            if (err == ESP_OK) {
                printf("CAN RX ID=0x%03lX DLC=%d data=%02X %02X %02X %02X\n", (unsigned long)rx.id,
                       rx.dlc, rx.data[0], rx.data[1], rx.data[2], rx.data[3]);
            } else {
                printf("CAN RX timeout/error: %s\n", esp_err_to_name(err));
                rc = 1;
            }
        }
    } else if (strcmp(sub, "pong") == 0) {
        printf("CAN listening for ping (%lu ms)...\n", (unsigned long)timeout_ms);
        twai_drv_frame_t rx = {0};
        err = twai_drv_receive(&rx, timeout_ms);
        if (err == ESP_OK) {
            printf("CAN RX ID=0x%03lX, replying 0x7E8...\n", (unsigned long)rx.id);
            twai_drv_frame_t tx = {
                .id = 0x7E8,
                .dlc = 8,
                .data = {0x06, 0x41, 0x00, 0xBF, 0x9F, 0xA8, 0x93, 0xAA},
            };
            err = twai_drv_send(&tx, 500);
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
        twai_drv_frame_t rx = {0};
        err = twai_drv_receive(&rx, timeout_ms);
        if (err == ESP_OK) {
            printf("CAN RX ID=0x%03lX ext=%d DLC=%d\n", (unsigned long)rx.id, rx.extd ? 1 : 0,
                   rx.dlc);
        } else {
            printf("CAN RX: %s\n", esp_err_to_name(err));
            rc = 1;
        }
    } else {
        printf("Unknown subcommand '%s'\n", sub);
        rc = 1;
    }

    (void)twai_drv_deinit();
    return rc;
}

static int cmd_kline(int argc, char **argv) {
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
        (void)uart_set_pin(KLINE_UART_PORT, BSP_PIN_KLINE_TX, BSP_PIN_KLINE_RX, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
        (void)uart_flush_input(KLINE_UART_PORT);

        const uint8_t tx_bytes[4] = {0x81, 0x10, 0xF1, 0x81};
        uint8_t rx_bytes[4] = {0};
        int written = uart_write_bytes(KLINE_UART_PORT, tx_bytes, sizeof(tx_bytes));
        (void)uart_wait_tx_done(KLINE_UART_PORT, pdMS_TO_TICKS(100));
        int read_len =
            uart_read_bytes(KLINE_UART_PORT, rx_bytes, sizeof(rx_bytes), pdMS_TO_TICKS(100));

        printf("K-Line TX %d bytes, RX echo %d bytes:", written, read_len);
        for (int i = 0; i < read_len; ++i) {
            printf(" %02X", rx_bytes[i]);
        }
        bool match = (read_len == (int)sizeof(tx_bytes)) &&
                     (memcmp(tx_bytes, rx_bytes, sizeof(tx_bytes)) == 0);
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

static int cmd_proto(int argc, char **argv) {
    (void)argc;
    (void)argv;
    power_mgmt_notify_activity();

    obd2_proto_id_t detected = OBD2_PROTO_AUTO;
    esp_err_t err = obd2_autodetect_run(&detected);
    if (err == ESP_OK) {
        printf("ATSP0 detected: %s (%d)\n", obd2_proto_name(detected), (int)detected);
        return 0;
    }
    printf("ATSP0 no ECU found: %s\n", esp_err_to_name(err));
    return 1;
}

static int cmd_sim(int argc, char **argv) {
    ecu_sim_state_t *sim = ecu_simulator_state();
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        ecu_simulator_lock();
        printf("ECU Sim: RPM=%u Speed=%u km/h Coolant=%d C O2=%u mV V_BAT=%lu mV DTCs=%u VIN=%s\n",
               (unsigned)sim->rpm, (unsigned)sim->speed_kmh, (int)sim->coolant_c,
               (unsigned)sim->o2_mv, (unsigned long)sim->vbat_mv, (unsigned)sim->dtc_count,
               sim->vin);
        ecu_simulator_unlock();
        return 0;
    }

    if (strcmp(argv[1], "clear") == 0) {
        ecu_simulator_lock();
        ecu_sim_clear_dtcs(sim);
        ecu_simulator_unlock();
        printf("ECU Sim: DTCs cleared\n");
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) {
        ecu_simulator_stop_bus();
        printf("ECU Sim: physical bus mode stopped\n");
        return 0;
    }
    if (strcmp(argv[1], "start") == 0) {
        bool can = (argc < 3 || strcmp(argv[2], "can") == 0 || strcmp(argv[2], "both") == 0);
        bool kline = (argc < 3 || strcmp(argv[2], "kline") == 0 || strcmp(argv[2], "both") == 0);
        esp_err_t err = ecu_simulator_start_bus(can, kline);
        printf("ECU Sim: physical bus mode %s (%s)\n", (err == ESP_OK) ? "started" : "failed",
               esp_err_to_name(err));
        return (err == ESP_OK) ? 0 : 1;
    }

    if (argc < 3) {
        printf("Usage: sim <status|rpm <v>|speed <v>|coolant <v>|o2 <mv>|vbat <mv>|dtc "
               "<P0100>|clear|start [can|kline|both]|stop>\n");
        return 1;
    }

    if (strcmp(argv[1], "rpm") == 0) {
        long val = 0;
        if (!parse_long_arg(argv[2], &val) || val < 0 || val > 16383) {
            printf("Error: RPM must be an integer 0..16383\n");
            return 1;
        }
        ecu_simulator_lock();
        ecu_sim_set_rpm(sim, (uint16_t)val);
        ecu_simulator_unlock();
        printf("ECU Sim: RPM=%ld\n", val);
    } else if (strcmp(argv[1], "speed") == 0) {
        long val = 0;
        if (!parse_long_arg(argv[2], &val) || val < 0 || val > 255) {
            printf("Error: speed must be an integer 0..255 km/h\n");
            return 1;
        }
        ecu_simulator_lock();
        ecu_sim_set_speed(sim, (uint8_t)val);
        ecu_simulator_unlock();
        printf("ECU Sim: Speed=%ld km/h\n", val);
    } else if (strcmp(argv[1], "coolant") == 0) {
        long val = 0;
        if (!parse_long_arg(argv[2], &val) || val < -40 || val > 215) {
            printf("Error: coolant must be an integer -40..215 C\n");
            return 1;
        }
        ecu_simulator_lock();
        ecu_sim_set_coolant(sim, (int16_t)val);
        ecu_simulator_unlock();
        printf("ECU Sim: Coolant=%ld C\n", val);
    } else if (strcmp(argv[1], "dtc") == 0) {
        ecu_simulator_lock();
        int rc = ecu_sim_inject_dtc(sim, argv[2]);
        uint8_t cnt = sim->dtc_count;
        ecu_simulator_unlock();
        if (rc != 0) {
            printf("Error: invalid DTC '%s' or table full\n", argv[2]);
            return 1;
        }
        printf("ECU Sim: injected DTC %s (count=%u)\n", argv[2], (unsigned)cnt);
    } else if (strcmp(argv[1], "vbat") == 0) {
        long val = 0;
        if (!parse_long_arg(argv[2], &val) || val < 0 || val > 24000) {
            printf("Error: vbat must be an integer 0..24000 mV\n");
            return 1;
        }
        bsp_vbat_set_sim_mv((uint32_t)val);
        ecu_simulator_lock();
        sim->vbat_mv = (uint32_t)val;
        ecu_simulator_unlock();
        printf("ECU Sim: simulated V_BAT=%ld mV\n", val);
    } else if (strcmp(argv[1], "o2") == 0) {
        long val = 0;
        if (!parse_long_arg(argv[2], &val) || val < 0 || val > 1275) {
            printf("Error: o2 must be an integer 0..1275 mV\n");
            return 1;
        }
        ecu_simulator_lock();
        ecu_sim_set_o2_mv(sim, (uint16_t)val);
        ecu_simulator_unlock();
        printf("ECU Sim: O2=%ld mV\n", val);
    } else {
        printf("Unknown subcommand '%s'\n", argv[1]);
        return 1;
    }
    return 0;
}

static void register_commands(void) {
    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "Print chip, memory, and BSP GPIO info",
            .hint = NULL,
            .func = &cmd_info,
        },
        {
            .command = "at",
            .help = "Send an ELM327 AT or hex OBD/UDS command over USB transport: at <cmd>",
            .hint = NULL,
            .func = &cmd_at,
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
            .command = "power",
            .help = "Power telemetry & sleep governance: power [sleep|selftest]",
            .hint = NULL,
            .func = &cmd_power,
        },
        {
            .command = "can",
            .help = "500 kbps TWAI bringup: can <ping|pong|rx|probe> [timeout_ms]",
            .hint = NULL,
            .func = &cmd_can,
        },
        {
            .command = "kline",
            .help = "10,400 baud K-Line bringup: kline <echo|pulse>",
            .hint = NULL,
            .func = &cmd_kline,
        },
        {
            .command = "proto",
            .help = "Run ATSP0 auto-protocol detection (CAN + K-Line)",
            .hint = NULL,
            .func = &cmd_proto,
        },
        {
            .command = "sim",
            .help = "ECU simulator control: sim <status|rpm <v>|speed <v>|coolant <v>|vbat "
                    "<mv>|dtc <P0100>|clear|start|stop>",
            .hint = NULL,
            .func = &cmd_sim,
        },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

esp_err_t cli_start(void) {
    (void)transport_stream_register_tx(TRANSPORT_ID_USB, usb_cli_transport_tx, NULL);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "obd2> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
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
