set(srcs stm32_comm.c)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_uart esp_timer)
