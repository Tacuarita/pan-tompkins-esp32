#include "replay_cpu.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"

typedef struct {
    void *task;
    int64_t since_us;
} RunningTask;

static portMUX_TYPE cpu_lock = portMUX_INITIALIZER_UNLOCKED;
static RunningTask running[portNUM_PROCESSORS];
static TaskHandle_t measured[REPLAY_CPU_TASKS];
static uint64_t retired[REPLAY_CPU_TASKS];
static int64_t start_us;
static int64_t end_us;
static bool active;

static uint64_t IRAM_ATTR overlap(int64_t begin, int64_t end)
{
    begin = begin < start_us ? start_us : begin;
    end = end > end_us ? end_us : end;
    return end > begin ? (uint64_t)(end - begin) : 0;
}

void IRAM_ATTR replay_cpu_switch_in(void *task)
{
    unsigned core = xPortGetCoreID();
    portENTER_CRITICAL(&cpu_lock);
    running[core] = (RunningTask){task, esp_timer_get_time()};
    portEXIT_CRITICAL(&cpu_lock);
}

void IRAM_ATTR replay_cpu_switch_out(void *task)
{
    unsigned core = xPortGetCoreID();
    portENTER_CRITICAL(&cpu_lock);
    if (active && running[core].task == task) {
        for (unsigned index = 0; index < REPLAY_CPU_TASKS; ++index) {
            if (task == measured[index]) {
                retired[index] += overlap(running[core].since_us, esp_timer_get_time());
                break;
            }
        }
    }
    running[core].task = NULL;
    portEXIT_CRITICAL(&cpu_lock);
}

void replay_cpu_begin(const TaskHandle_t tasks[REPLAY_CPU_TASKS], int64_t start, int64_t end)
{
    portENTER_CRITICAL(&cpu_lock);
    memcpy(measured, tasks, sizeof(measured));
    memset(retired, 0, sizeof(retired));
    start_us = start;
    end_us = end;
    active = true;
    portEXIT_CRITICAL(&cpu_lock);
}

void replay_cpu_snapshot(int64_t boundary_us, uint64_t counters[REPLAY_CPU_TASKS])
{
    portENTER_CRITICAL(&cpu_lock);
    // At the start the window is empty, even if the observer woke late.
    memset(counters, 0, sizeof(retired));
    if (boundary_us == end_us) {
        memcpy(counters, retired, sizeof(retired));
        for (unsigned core = 0; core < portNUM_PROCESSORS; ++core) {
            for (unsigned index = 0; index < REPLAY_CPU_TASKS; ++index) {
                if (running[core].task == measured[index]) {
                    counters[index] += overlap(running[core].since_us, boundary_us);
                }
            }
        }
    }
    portEXIT_CRITICAL(&cpu_lock);
}

void replay_cpu_end(void)
{
    portENTER_CRITICAL(&cpu_lock);
    active = false;
    portEXIT_CRITICAL(&cpu_lock);
}
