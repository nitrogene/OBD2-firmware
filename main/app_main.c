#include "bsp.h"
#include "cli_commands.h"

#include "esp_log.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Booting OBD2-firmware (Phase 1 BSP + MODE_BRINGUP)");

    ESP_ERROR_CHECK(bsp_init());
    bsp_led_set_pattern(BSP_LED_BREATHING);

    ESP_ERROR_CHECK(cli_start());
}
