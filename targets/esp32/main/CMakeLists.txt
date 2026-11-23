set(SECOND_WORKER_CORE 1 CACHE STRING "Core for detector worker 2 (A=1, B=0)")
if(NOT SECOND_WORKER_CORE MATCHES "^[01]$")
    message(FATAL_ERROR "SECOND_WORKER_CORE must be 1 (A) or 0 (B)")
endif()

idf_component_register(SRCS replay_uart.c replay_ring.c replay_cpu.c ../../../src/pan_tompkins.c
                            ../../../src/beat_fusion.c
                       INCLUDE_DIRS .
                       PRIV_INCLUDE_DIRS ../../../src
                       PRIV_REQUIRES esp_driver_uart esp_timer esp_rom)
target_compile_definitions(${COMPONENT_LIB} PRIVATE SECOND_WORKER_CORE=${SECOND_WORKER_CORE})
