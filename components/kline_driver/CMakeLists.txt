idf_component_register(
    SRCS "kline_proto.c" "kline_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp driver esp_timer
)
