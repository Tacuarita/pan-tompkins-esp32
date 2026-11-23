#include "replay_ring.h"
#include "beat_fusion.h"

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
    FUSION_PRIORITY = 4,
    FUSION_STACK_BYTES = 4096,
    FUSION_WINDOW = 13,
    FUSION_MAX_DELAY = 128,
    FUSION_PROGRESS = 1,
    FUSION_STOP = 2,
    TEST_HANDOFF_DELAY_MS = 12,
};

_Static_assert((int)FUSION_LEADS == (int)FRAME_LEADS, "Fusion must read every lead");

typedef enum {
    NORMAL,
    INJECT_UNDERRUN,
    INJECT_OVERFLOW,
    INJECT_HANDOFF_DELAY,
} TrialMode;

typedef enum {
    REPORT_RESULT,
    REPORT_CANDIDATES,
    REPORT_FUSION,
} ReportMode;

typedef struct {
    PtDetector detector;
    PtCandidate *candidates;
    _Atomic size_t candidate_count;
} Lead;

typedef struct {
    Lead leads[LEADS_PER_WORKER];
    size_t candidate_capacity;
    bool candidate_overflow;
    unsigned id;
    bool stalled;
    bool delay_handoffs;
    uint32_t delayed_handoffs;
    int core;
    TaskHandle_t task;
    TaskHandle_t fusion_task;
    uint32_t frames;
    uint32_t index_errors;
    uint32_t crc;
} Worker;

typedef struct {
    BeatFusion fusion;
    uint32_t *locations;
    size_t count;
} FusionTrial;

static ReplayRing ring;
static Worker workers[WORKER_COUNT];
static _Atomic bool stop_workers;
static SemaphoreHandle_t workers_done;

static void replay_worker(void *argument)
{
    Worker *worker = argument;
    worker->core = xPortGetCoreID();
    bool stopping;
    do {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        stopping = atomic_load_explicit(&stop_workers, memory_order_acquire);
        const ReplayFrame *frame;
        while (!worker->stalled && (frame = replay_ring_peek(&ring, worker->id)) != NULL) {
            worker->index_errors += frame->index != worker->frames;
            uint8_t triplet[2 * LEADS_PER_WORKER];
            bool produced = false;
            for (unsigned lane = 0; lane < LEADS_PER_WORKER; ++lane) {
                int16_t sample = frame->samples[worker->id * LEADS_PER_WORKER + lane];
                uint16_t code = (uint16_t)sample;
                triplet[2 * lane] = code & 0xff;
                triplet[2 * lane + 1] = code >> 8;

                Lead *lead = &worker->leads[lane];
                PtCandidate candidate;
                if (pt_process(&lead->detector, sample, &candidate)) {
                    produced = true;
                    size_t count = atomic_load_explicit(&lead->candidate_count, memory_order_relaxed);
                    if (count == worker->candidate_capacity) {
                        worker->candidate_overflow = true;
                    } else {
                        lead->candidates[count] = candidate;
                        atomic_store_explicit(&lead->candidate_count, count + 1, memory_order_release);
                    }
                }
            }
            worker->crc = esp_rom_crc32_le(worker->crc, triplet, sizeof(triplet));
            if (worker->delay_handoffs && produced) {
                ++worker->delayed_handoffs;
                vTaskDelay(pdMS_TO_TICKS(TEST_HANDOFF_DELAY_MS));
            }
            ++worker->frames;
            replay_ring_finish(&ring, worker->id);
            xTaskNotify(worker->fusion_task, FUSION_PROGRESS, eSetBits);
        }
    } while (!stopping);
    xSemaphoreGive(workers_done);
    vTaskSuspend(NULL);
}

static void retain_fused_beat(void *context, uint32_t location)
{
    FusionTrial *trial = context;
    trial->locations[trial->count++] = location;
}

static void replay_fusion(void *argument)
{
    FusionTrial *trial = argument;
    uint32_t events;
    do {
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        // Snapshot progress first. Append-only arrays can also contain later frames.
        uint32_t completed = replay_ring_completed(&ring);
        FusionLead leads[FUSION_LEADS];
        for (unsigned id = 0; id < WORKER_COUNT; ++id) {
            for (unsigned lane = 0; lane < LEADS_PER_WORKER; ++lane) {
                const Lead *lead = &workers[id].leads[lane];
                leads[id * LEADS_PER_WORKER + lane] = (FusionLead){
                    lead->candidates,
                    atomic_load_explicit(&lead->candidate_count, memory_order_acquire),
                };
            }
        }
        beat_fusion_advance(&trial->fusion, leads, completed, retain_fused_beat, trial);
    } while ((events & FUSION_STOP) == 0);
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

static void write_candidates(void)
{
    char line[80];
    snprintf(line, sizeof(line), "PLACEMENT %d %d %d\n",
             (int)xPortGetCoreID(), workers[0].core, workers[1].core);
    write_line(line);
    for (unsigned id = 0; id < WORKER_COUNT; ++id) {
        for (unsigned lane = 0; lane < LEADS_PER_WORKER; ++lane) {
            const Lead *lead = &workers[id].leads[lane];
            size_t count = atomic_load_explicit(&lead->candidate_count, memory_order_acquire);
            for (size_t index = 0; index < count; ++index) {
                PtCandidate candidate = lead->candidates[index];
                snprintf(line, sizeof(line), "CANDIDATE %u %" PRIu32 " %" PRIu32 "\n",
                         id * LEADS_PER_WORKER + lane, candidate.beat_location,
                         candidate.production_frame);
                write_line(line);
            }
        }
    }
}

static void write_fusion(const FusionTrial *trial)
{
    char line[96];
    snprintf(line, sizeof(line), "FUSION %" PRIu32 " %" PRIu32 " %" PRIu64 " %" PRIu32 "\n",
             trial->fusion.window, trial->fusion.max_delay, trial->fusion.late_count,
             workers[1].delayed_handoffs);
    write_line(line);
    for (size_t i = 0; i < trial->count; ++i) {
        snprintf(line, sizeof(line), "FUSED %" PRIu32 "\n", trial->locations[i]);
        write_line(line);
    }
}

static void run_trial(esp_timer_handle_t timer, uint32_t count, TrialMode mode, ReportMode report)
{
    uint32_t max_frames = count;
    if (mode == INJECT_UNDERRUN) {
        max_frames = 1;
    } else if (mode == INJECT_OVERFLOW) {
        max_frames = RING_CAPACITY;
    }
    // Accepted locations are inside the replay and separated by at least the refractory interval.
    size_t capacity = 1 + (max_frames - 1) / PT_REFRACTORY_SAMPLES;
    PtCandidate *candidates = calloc(FRAME_LEADS * capacity, sizeof(*candidates));
    FusionTrial fusion = {
        .fusion = {.window = FUSION_WINDOW, .max_delay = FUSION_MAX_DELAY},
        // A fused beat consumes at least two distinct candidates.
        .locations = calloc(FRAME_LEADS * capacity / 2, sizeof(*fusion.locations)),
    };
    if (candidates == NULL || fusion.locations == NULL) {
        free(candidates);
        free(fusion.locations);
        write_line("ERROR CANDIDATE_STORAGE\n");
        return;
    }

    replay_ring_reset(&ring);
    atomic_store_explicit(&stop_workers, false, memory_order_relaxed);
    memset(workers, 0, sizeof(workers));
    for (unsigned id = 0; id < WORKER_COUNT; ++id) {
        workers[id].id = id;
        workers[id].stalled = mode == INJECT_OVERFLOW && id == 1;
        workers[id].delay_handoffs = mode == INJECT_HANDOFF_DELAY && id == 1;
        workers[id].candidate_capacity = capacity;
        for (unsigned lane = 0; lane < LEADS_PER_WORKER; ++lane) {
            workers[id].leads[lane].candidates =
                &candidates[(id * LEADS_PER_WORKER + lane) * capacity];
            atomic_init(&workers[id].leads[lane].candidate_count, 0);
        }
    }
    TaskHandle_t fusion_task;
    if (xTaskCreatePinnedToCore(replay_fusion, "beat_fusion", FUSION_STACK_BYTES, &fusion,
                               FUSION_PRIORITY, &fusion_task, 0) != pdPASS) {
        abort();
    }
    for (unsigned id = 0; id < WORKER_COUNT; ++id) {
        workers[id].fusion_task = fusion_task;
        BaseType_t created = xTaskCreatePinnedToCore(replay_worker, "lead_worker",
                                                     WORKER_STACK_BYTES, &workers[id],
                                                     WORKER_PRIORITY, &workers[id].task,
                                                     id == 0 ? 1 : SECOND_WORKER_CORE);
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

            if ((mode == NORMAL || mode == INJECT_HANDOFF_DELAY)
                && (published + 1) % GRANT_FRAMES == 0 && granted < count) {
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
        if (workers[id].candidate_overflow) {
            result = "CANDIDATE_OVERFLOW";
        }
    }
    // The final fusion pass must see every real frame the workers drained.
    xTaskNotify(fusion_task, FUSION_STOP, eSetBits);
    xSemaphoreTake(workers_done, portMAX_DELAY);
    vTaskDelete(fusion_task);

    char summary[256];
    snprintf(summary, sizeof(summary),
             "RESULT %s %" PRIu32 " %" PRId64 " %" PRId64 " %" PRId64
             " %" PRIu32 " %" PRIu32 " %08" PRIx32 " %" PRIu32 " %" PRIu32 " %08" PRIx32 "\n",
             result, published, scheduled_span_us, actual_span_us, max_late_us,
             workers[0].frames, workers[0].index_errors, workers[0].crc,
             workers[1].frames, workers[1].index_errors, workers[1].crc);
    ESP_ERROR_CHECK(uart_flush_input(REPLAY_UART));
    if (report != REPORT_RESULT && strcmp(result, "OK") == 0) {
        write_candidates();
        if (report == REPORT_FUSION) {
            write_fusion(&fusion);
        }
    }
    free(fusion.locations);
    free(candidates);
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
        run_trial(timer, count, NORMAL, REPORT_RESULT);
    } else if (strcmp(end, " parity") == 0) {
        run_trial(timer, count, NORMAL, REPORT_CANDIDATES);
    } else if (strcmp(end, " fusion") == 0) {
        run_trial(timer, count, NORMAL, REPORT_FUSION);
    } else if (strcmp(end, " fusion-delay") == 0) {
        run_trial(timer, count, INJECT_HANDOFF_DELAY, REPORT_FUSION);
    } else if (count >= 2 && strcmp(end, " underrun") == 0) {
        run_trial(timer, count, INJECT_UNDERRUN, REPORT_RESULT);
    } else if (count > RING_CAPACITY && strcmp(end, " overflow") == 0) {
        run_trial(timer, count, INJECT_OVERFLOW, REPORT_RESULT);
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
