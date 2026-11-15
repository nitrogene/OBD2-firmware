idf_component_register(
    SRCS "app_main.c" "cli_commands.c"
    INCLUDE_DIRS "."
    REQUIRES bsp console driver esp_psram esp_timer spi_flash
)
