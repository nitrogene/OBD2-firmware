idf_component_register(
    SRCS "obd2_codec.c" "obd2_autodetect.c"
    INCLUDE_DIRS "include"
    REQUIRES isotp kline_driver nvs_flash twai_driver
)
