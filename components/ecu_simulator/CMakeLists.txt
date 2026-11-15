idf_component_register(
    SRCS "ecu_sim_core.c" "ecu_simulator.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos isotp kline_driver log obd2_stack twai_driver
)
