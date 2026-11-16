idf_component_register(
    SRCS "virtual_bench_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES driver ecu_simulator esp_lcd freertos log power_mgmt transport_stream
)
