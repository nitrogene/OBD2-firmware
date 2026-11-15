idf_component_register(
    SRCS "kwp2000.c"
    INCLUDE_DIRS "include"
    REQUIRES kline_driver obd2_stack
)
