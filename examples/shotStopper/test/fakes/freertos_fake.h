// Host-side fake of the FreeRTOS calls the sketch uses (ESP32 Arduino core
// flavour). Tasks are recorded but never started: tests drive task bodies
// themselves, interleaved with loop(), on the simulated clock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
struct FakeQueue;
typedef FakeQueue* QueueHandle_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdPASS pdTRUE
#define pdFAIL pdFALSE
#define errQUEUE_FULL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))  // configTICK_RATE_HZ is 1000 on the ESP32 core

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char* name, uint32_t stackDepth, void* parameters,
                                   UBaseType_t priority, TaskHandle_t* createdTask, BaseType_t coreId);
void vTaskDelay(TickType_t ticks);

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize);
// The wait is ignored: a full queue fails at once and an empty one returns
// pdFALSE at once (the host is single-threaded, nothing could fill it).
BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);

namespace fake {

struct TaskRecord {
  std::string name;
  TaskFunction_t function;
  void* parameters;
  uint32_t stackDepth;
  UBaseType_t priority;
  BaseType_t coreId;
};

const std::vector<TaskRecord>& tasks();

}  // namespace fake
