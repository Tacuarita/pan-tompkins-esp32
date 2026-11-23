#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { REPLAY_CPU_TASKS = 4 };

void replay_cpu_begin(const TaskHandle_t tasks[REPLAY_CPU_TASKS], int64_t start_us, int64_t end_us);
void replay_cpu_snapshot(int64_t boundary_us, uint64_t counters[REPLAY_CPU_TASKS]);
void replay_cpu_end(void);
