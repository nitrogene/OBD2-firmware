#include "virtual_bench_ui.h"

#include <stdio.h>
#include <string.h>

#ifdef CONFIG_OBD2_VIRTUAL_BENCH

#include "bsp.h"
#include "driver/gpio.h"
#include "ecu_simulator.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_mgmt.h"
#include "transport_stream.h"

static const char *TAG = "vbench_ui";

/* LilyGO TTGO T-Display S3 ST7789 8-bit parallel pins */
#define LCD_PIN_POWER_ON 15
#define LCD_PIN_BL 38
#define LCD_PIN_RD 9
#define LCD_PIN_CS 6
#define LCD_PIN_DC 7
#define LCD_PIN_WR 8
#define LCD_PIN_RST 5
#define LCD_PIN_D0 39
#define LCD_PIN_D1 40
#define LCD_PIN_D2 41
#define LCD_PIN_D3 42
#define LCD_PIN_D4 45
#define LCD_PIN_D5 46
#define LCD_PIN_D6 47
#define LCD_PIN_D7 48

#define BTN_PIN_SCENARIO 0
#define BTN_PIN_FAULT 14

#define LCD_W 320
#define LCD_H 170
#define BAND_H 20

/* RGB565 colors (byte-swapped for little-endian i80 bus if needed; ST7789 BGR/RGB) */
#define COL_BG 0x0000U
#define COL_HDR_BG 0x0210U
#define COL_WHITE 0xFFFFU
#define COL_CYAN 0x07FFU
#define COL_GREEN 0x07E0U
#define COL_YELLOW 0xFFE0U

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_band_buf = NULL;
static SemaphoreHandle_t s_dma_done_sem = NULL;
static char s_last_cmd[36] = "READY";

static bool vbench_on_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp_woken = pdFALSE;
    if (s_dma_done_sem != NULL) {
        xSemaphoreGiveFromISR(s_dma_done_sem, &hp_woken);
    }
    return hp_woken == pdTRUE;
}

/* Minimal 5x7 ASCII font (0x20..0x5F, uppercase + digits + punctuation) */
static const uint8_t s_font5x7[64][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* '_' */
};

static void flush_band(int y_start, int height) {
    (void)esp_lcd_panel_draw_bitmap(s_panel, 0, y_start, LCD_W, y_start + height, s_band_buf);
    if (s_dma_done_sem != NULL) {
        (void)xSemaphoreTake(s_dma_done_sem, pdMS_TO_TICKS(100));
    }
}

static void draw_text_band(int y_start, const char *text, uint16_t fg, uint16_t bg) {
    if (s_panel == NULL || s_band_buf == NULL) {
        return;
    }
    for (int i = 0; i < LCD_W * BAND_H; ++i) {
        s_band_buf[i] = bg;
    }

    int x = 6;
    for (size_t idx = 0; text[idx] != '\0' && x + 10 < LCD_W; ++idx) {
        char c = text[idx];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        uint8_t gi = (c >= 0x20 && c <= 0x5F) ? (uint8_t)(c - 0x20) : 0U;
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = s_font5x7[gi][col];
            for (int row = 0; row < 7; ++row) {
                if ((bits >> row) & 1U) {
                    int px = x + col * 2;
                    int py = 3 + row * 2;
                    s_band_buf[py * LCD_W + px] = fg;
                    s_band_buf[py * LCD_W + px + 1] = fg;
                    s_band_buf[(py + 1) * LCD_W + px] = fg;
                    s_band_buf[(py + 1) * LCD_W + px + 1] = fg;
                }
            }
        }
        x += 12;
    }

    flush_band(y_start, BAND_H);
}

static const char *transport_name(transport_id_t id) {
    switch (id) {
    case TRANSPORT_ID_BLE:
        return "BLE";
    case TRANSPORT_ID_WIFI:
        return "WIFI";
    case TRANSPORT_ID_USB:
        return "USB";
    default:
        return "IDLE";
    }
}

static void vbench_poll_buttons(void) {
    static int prev_b0 = 1;
    static int prev_b1 = 1;
    static uint8_t scenario_idx = 0U;
    static uint8_t fault_idx = 0U;

    int b0 = gpio_get_level(BTN_PIN_SCENARIO);
    if (prev_b0 == 1 && b0 == 0) {
        static const struct {
            const char *name;
            uint16_t rpm;
            uint8_t speed;
            int16_t coolant;
            uint32_t vbat_mv;
        } scenarios[] = {
            {"IDLE", 850U, 0U, 88, 12600U},
            {"CRUISE", 2400U, 90U, 90, 14100U},
            {"ACCEL", 4200U, 130U, 94, 14200U},
            {"OVERHEAT", 1100U, 20U, 112, 13800U},
        };
        scenario_idx = (uint8_t)((scenario_idx + 1U) % 4U);
        ecu_sim_state_t *sim = ecu_simulator_state();
        ecu_sim_set_rpm(sim, scenarios[scenario_idx].rpm);
        ecu_sim_set_speed(sim, scenarios[scenario_idx].speed);
        ecu_sim_set_coolant(sim, scenarios[scenario_idx].coolant);
        bsp_vbat_set_sim_mv(scenarios[scenario_idx].vbat_mv);
        sim->vbat_mv = scenarios[scenario_idx].vbat_mv;
        snprintf(s_last_cmd, sizeof(s_last_cmd), "BTN0:%s", scenarios[scenario_idx].name);
    }
    prev_b0 = b0;

    int b1 = gpio_get_level(BTN_PIN_FAULT);
    if (prev_b1 == 1 && b1 == 0) {
        fault_idx = (uint8_t)((fault_idx + 1U) % 3U);
        ecu_sim_state_t *sim = ecu_simulator_state();
        if (fault_idx == 0U) {
            ecu_sim_clear_dtcs(sim);
            bsp_vbat_set_sim_mv(12600U);
            sim->vbat_mv = 12600U;
            snprintf(s_last_cmd, sizeof(s_last_cmd), "BTN1:CLEAN");
        } else if (fault_idx == 1U) {
            (void)ecu_sim_inject_dtc(sim, "P0301");
            snprintf(s_last_cmd, sizeof(s_last_cmd), "BTN1:DTC P0301");
        } else {
            bsp_vbat_set_sim_mv(11200U);
            sim->vbat_mv = 11200U;
            snprintf(s_last_cmd, sizeof(s_last_cmd), "BTN1:LOW 11.2V");
        }
    }
    prev_b1 = b1;
}

static void vbench_ui_task(void *arg) {
    (void)arg;
    char line[48];

    /* Clear screen once */
    for (int y = 0; y < LCD_H; y += BAND_H) {
        int h = (y + BAND_H <= LCD_H) ? BAND_H : (LCD_H - y);
        for (int i = 0; i < LCD_W * h; ++i) {
            s_band_buf[i] = COL_BG;
        }
        flush_band(y, h);
    }

    for (;;) {
        vbench_poll_buttons();
        const ecu_sim_state_t *sim = ecu_simulator_state();
        power_status_t pwr = {0};
        (void)power_mgmt_get_status(&pwr);

        snprintf(line, sizeof(line), "OBD2 BENCH  [%s]",
                 transport_name(transport_stream_active_id()));
        draw_text_band(0, line, COL_CYAN, COL_HDR_BG);

        snprintf(line, sizeof(line), "RPM:%-5u SPD:%-3u", (unsigned)sim->rpm,
                 (unsigned)sim->speed_kmh);
        draw_text_band(28, line, COL_WHITE, COL_BG);

        snprintf(line, sizeof(line), "CLT:%-4dC BAT:%lu.%luV", (int)sim->coolant_c,
                 (unsigned long)(pwr.vbat_mv / 1000U),
                 (unsigned long)((pwr.vbat_mv % 1000U + 50U) / 100U));
        draw_text_band(56, line, COL_GREEN, COL_BG);

        snprintf(line, sizeof(line), "DTC:%-2u    MODE:SIM", (unsigned)sim->dtc_count);
        draw_text_band(84, line, (sim->dtc_count > 0U) ? COL_YELLOW : COL_WHITE, COL_BG);

        snprintf(line, sizeof(line), "CMD:%s", s_last_cmd);
        draw_text_band(120, line, COL_CYAN, COL_BG);

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t virtual_bench_ui_init(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_PIN_SCENARIO) | (1ULL << BTN_PIN_FAULT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    /* Power on LCD + keep RD high + turn on backlight */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_POWER_ON) | (1ULL << LCD_PIN_RD) | (1ULL << LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(LCD_PIN_POWER_ON, 1);
    gpio_set_level(LCD_PIN_RD, 1);
    gpio_set_level(LCD_PIN_BL, 1);

    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = LCD_PIN_DC,
        .wr_gpio_num = LCD_PIN_WR,
        .data_gpio_nums =
            {
                LCD_PIN_D0,
                LCD_PIN_D1,
                LCD_PIN_D2,
                LCD_PIN_D3,
                LCD_PIN_D4,
                LCD_PIN_D5,
                LCD_PIN_D6,
                LCD_PIN_D7,
            },
        .bus_width = 8,
        .max_transfer_bytes = LCD_W * BAND_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    s_dma_done_sem = xSemaphoreCreateBinary();
    if (s_dma_done_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
        .on_color_trans_done = vbench_on_trans_done,
        .dc_levels = {.dc_idle_level = 0,
                      .dc_cmd_level = 0,
                      .dc_dummy_level = 0,
                      .dc_data_level = 1},
        .flags = {.swap_color_bytes = 1},
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 35));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_band_buf = heap_caps_malloc(LCD_W * BAND_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (s_band_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(vbench_ui_task, "vbench_ui", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LilyGO TTGO T-Display S3 ST7789 UI started (320x170)");
    return ESP_OK;
}

void virtual_bench_ui_set_last_cmd(const char *text) {
    if (text != NULL) {
        snprintf(s_last_cmd, sizeof(s_last_cmd), "%s", text);
    }
}

#else /* !CONFIG_OBD2_VIRTUAL_BENCH */

esp_err_t virtual_bench_ui_init(void) {
    return ESP_OK;
}

void virtual_bench_ui_set_last_cmd(const char *text) {
    (void)text;
}

#endif /* CONFIG_OBD2_VIRTUAL_BENCH */
