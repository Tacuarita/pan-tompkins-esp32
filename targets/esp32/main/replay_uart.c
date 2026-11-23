#include "replay_ring.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    REPLAY_UART = UART_NUM_0,
    UART_RX_BYTES = 4096,
    UART_TX_BYTES = 1024,
    PREFILL_FRAMES = 128,
    GRANT_FRAMES = 32,
    WORKER_PRIORITY = 5,
    WORKER_STACK_BYTES = 4096,
};

typedef enum {
    NORMAL,
    INJECT_UNDERRUN,
    INJECT_OVERFLOW,
} TrialMode;

typedef struct {
    unsigned id;
    bool stalled;
    TaskHandle_t task;
    uint32_t frames;
    uint32_t index_errors;
    uint32_t crc;
} Worker;

static ReplayRing ring;
static Worker workers[WORKER_COUNT];
static _Atomic bool stop_workers;
static SemaphoreHandle_t workers_done;

static void replay_worker(void *argument)
{
    Worker *worker = argument;
    bool stopping;
    do {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        stopping = atomic_load_explicit(&stop_workers, memory_order_acquire);
        const ReplayFrame *frame;
        while (!worker->stalled && (frame = replay_ring_peek(&ring, worker->id)) != NULL) {
            worker->index_errors += frame->index != worker->frames;
            uint8_t triplet[2 * LEADS_PER_WORKER];
            for (unsigned lead = 0; lead < LEADS_PER_WORKER; ++lead) {
                uint16_t code = frame->samples[worker->id * LEADS_PER_WORKER + lead];
                triplet[2 * lead] = code & 0xff;
                triplet[2 * lead + 1] = code >> 8;
            }
            worker->crc = esp_rom_crc32_le(worker->crc, triplet, sizeof(triplet));
            ++worker->frames;
            replay_ring_finish(&ring, worker->id);
        }
    } while (!stopping);
    xSemaphoreGive(workers_done);
    vTaskSuspend(NULL);
}

static void wake_replay(void *task)
{
    xTaskNotifyGive(task);
}

static void wait_for_release(esp_timer_handle_t timer, int64_t scheduled_us)
{
    int64_t now_us = esp_timer_get_time();
    while (now_us < scheduled_us) {
        ESP_ERROR_CHECK(esp_timer_start_once(timer, scheduled_us - now_us));
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        now_us = esp_timer_get_time();
    }
}

static void write_line(const char *line)
{
    uart_write_bytes(REPLAY_UART, line, strlen(line));
}

static void grant_frames(uint32_t frames)
{
    char line[32];
    snprintf(line, sizeof(line), "GRANT %" PRIu32 "\n", frames);
    write_line(line);
}

static bool wait_for_prefill(uint32_t frames)
{
    int64_t timeout_us = esp_timer_get_time() + 5000000;
    do {
        size_t buffered;
        ESP_ERROR_CHECK(uart_get_buffered_data_len(REPLAY_UART, &buffered));
        if (buffered >= frames * FRAME_BYTES) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (esp_timer_get_time() < timeout_us);
    return false;
}

static void run_trial(esp_timer_handle_t timer, uint32_t count, TrialMode mode)
{
    replay_ring_reset(&ring);
    atomic_store_explicit(&stop_workers, false, memory_order_relaxed);
    memset(workers, 0, sizeof(workers));
    for (unsigned id = 0; id < WORKER_COUNT; ++id) {
        workers[id].id = id;
        workers[id].stalled = mode == INJECT_OVERFLOW && id == 1;
        BaseType_t created = xTaskCreatePinnedToCore(replay_worker, "lead_worker",
                                                     WORKER_STACK_BYTES, &workers[id],
                                                     WORKER_PRIORITY, &workers[id].task, 1);
        if (created != pdPASS) {
            abort();
        }
    }

    uint32_t prefill = PREFILL_FRAMES;
    if (mode == INJECT_UNDERRUN) {
        prefill = 1;
    } else if (mode == INJECT_OVERFLOW) {
        prefill = RING_CAPACITY + 1;
    }
    uint32_t granted = count < prefill ? count : prefill;
    grant_frames(granted);

    const char *result = "PREFILL_TIMEOUT";
    uint32_t published = 0;
    int64_t max_late_us = 0;
    int64_t actual_span_us = 0;
    int64_t scheduled_span_us = 0;
    if (wait_for_prefill(granted)) {
        result = "OK";
        int64_t start_us = esp_timer_get_time();
        int64_t first_release_us = start_us;
        for (; published < count; ++published) {
            scheduled_span_us = (int64_t)published * 1000000 / REPLAY_RATE_HZ;
            int64_t scheduled_us = start_us + scheduled_span_us;
            wait_for_release(timer, scheduled_us);

            uint8_t bytes[FRAME_BYTES];
            bool has_frame = uart_read_bytes(REPLAY_UART, bytes, sizeof(bytes), 0) == FRAME_BYTES;
            bool delivered = has_frame && replay_ring_publish(&ring, bytes);
            int64_t actual_us = esp_timer_get_time();
            if (published == 0) {
                first_release_us = actual_us;
            }
            actual_span_us = actual_us - first_release_us;
            if (actual_us - scheduled_us > max_late_us) {
                max_late_us = actual_us - scheduled_us;
            }

            if (!delivered) {
                result = has_frame ? "RING_OVERFLOW" : "INPUT_UNDERRUN";
                break;
            }
            for (unsigned id = 0; id < WORKER_COUNT; ++id) {
                if (!workers[id].stalled) {
                    xTaskNotifyGive(workers[id].task);
                }
            }

            if (mode == NORMAL && (published + 1) % GRANT_FRAMES == 0 && granted < count) {
                uint32_t available = count - granted;
                uint32_t next = available < GRANT_FRAMES ? available : GRANT_FRAMES;
                grant_frames(next);
                granted += next;
            }
        }
    }

    atomic_store_explicit(&stop_workers, true, memory_order_release);
    for (unsigned id = 0; id < WORKER_COUNT; ++id) {
        xTaskNotifyGive(workers[id].task);
    }
    for (unsigned id = 0; id < WORKER_COUNT; ++id) {
        xSemaphoreTake(workers_done, portMAX_DELAY);
    }
    for (unsigned id = 0; id < WORKER_COUNT; ++id) {
        vTaskDelete(workers[id].task);
    }

    char summary[256];
    snprintf(summary, sizeof(summary),
             "RESULT %s %" PRIu32 " %" PRId64 " %" PRId64 " %" PRId64
             " %" PRIu32 " %" PRIu32 " %08" PRIx32 " %" PRIu32 " %" PRIu32 " %08" PRIx32 "\n",
             result, published, scheduled_span_us, actual_span_us, max_late_us,
             workers[0].frames, workers[0].index_errors, workers[0].crc,
             workers[1].frames, workers[1].index_errors, workers[1].crc);
    ESP_ERROR_CHECK(uart_flush_input(REPLAY_UART));
    write_line(summary);
}

static void handle_command(esp_timer_handle_t timer, const char *command)
{
    if (strcmp(command, "PING") == 0) {
        write_line("READY 1\n");
        return;
    }

    if (strncmp(command, "START ", 6) != 0) {
        return;
    }
    const char *number = command + 6;
    char *end;
    errno = 0;
    uint32_t count = strtoul(number, &end, 10);
    if (number[0] < '1' || number[0] > '9' || errno == ERANGE) {
        return;
    }
    if (strcmp(end, " normal") == 0) {
        run_trial(timer, count, NORMAL);
    } else if (count >= 2 && strcmp(end, " underrun") == 0) {
        run_trial(timer, count, INJECT_UNDERRUN);
    } else if (count > RING_CAPACITY && strcmp(end, " overflow") == 0) {
        run_trial(timer, count, INJECT_OVERFLOW);
    }
}

void app_main(void)
{
    const uart_config_t uart = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(REPLAY_UART, &uart));
    ESP_ERROR_CHECK(uart_set_pin(REPLAY_UART, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(REPLAY_UART, UART_RX_BYTES, UART_TX_BYTES, 0, NULL, 0));

    workers_done = xSemaphoreCreateCounting(WORKER_COUNT, 0);
    if (workers_done == NULL) {
        abort();
    }
    const esp_timer_create_args_t timer_args = {
        .callback = wake_replay,
        .arg = xTaskGetCurrentTaskHandle(),
        .name = "ecg_release",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));

    char command[48];
    size_t length = 0;
    for (;;) {
        uint8_t byte;
        if (uart_read_bytes(REPLAY_UART, &byte, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (byte == '\n') {
            if (length < sizeof(command)) {
                command[length] = '\0';
                handle_command(timer, command);
            }
            length = 0;
        } else if (byte == '\0') {
            length = sizeof(command);
        } else if (length < sizeof(command)) {
            command[length++] = byte;
        }
    }
}
