cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pan_tompkins_replay)

# Attach the task-switch hooks to the ESP-IDF scheduler without modifying IDF.
idf_component_get_property(freertos_lib freertos COMPONENT_LIB)
target_compile_options(${freertos_lib} PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/main/replay_cpu_trace.h>")
