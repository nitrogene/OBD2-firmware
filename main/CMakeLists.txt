idf_component_register(
    SRCS "app_main.c" "cli_commands.c"
    INCLUDE_DIRS "."
    REQUIRES bsp comm_ble comm_wifi console driver ecu_simulator elm327_emu esp_psram esp_timer isotp kline_driver kwp2000 nvs_flash obd2_stack power_mgmt spi_flash transport_stream twai_driver virtual_bench_ui
)
