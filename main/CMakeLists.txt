idf_component_register(
    SRCS "app_main.c" "cli_commands.c"
    INCLUDE_DIRS "."
    REQUIRES bsp console driver esp_psram esp_timer power_mgmt spi_flash
)
