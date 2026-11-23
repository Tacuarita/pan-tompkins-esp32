idf_component_register(SRCS replay_uart.c replay_ring.c
                       INCLUDE_DIRS .
                       PRIV_REQUIRES esp_driver_uart esp_timer esp_rom)
