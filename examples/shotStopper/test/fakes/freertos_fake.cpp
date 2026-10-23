#include "freertos_fake.h"

#include <cstring>
#include <deque>

#include "Arduino.h"

struct FakeQueue {
  size_t length;
  size_t itemSize;
  std::deque<std::vector<uint8_t>> items;
};

namespace {

std::vector<fake::TaskRecord>& taskRecords() {
  static std::vector<fake::TaskRecord> records;
  return records;
}

std::vector<FakeQueue*>& queues() {  // owned for the life of the (forked) test process
  static std::vector<FakeQueue*> all;
  return all;
}

}  // namespace

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char* name, uint32_t stackDepth, void* parameters,
                                   UBaseType_t priority, TaskHandle_t* createdTask, BaseType_t coreId) {
  taskRecords().push_back({name ? name : "", task, parameters, stackDepth, priority, coreId});
  if (createdTask) *createdTask = reinterpret_cast<TaskHandle_t>(taskRecords().size());
  return pdPASS;
}

void vTaskDelay(TickType_t ticks) { delay(ticks); }

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize) {
  auto* q = new FakeQueue{length, itemSize, {}};
  queues().push_back(q);
  return q;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t wait) {
  (void)wait;
  if (!queue || queue->items.size() >= queue->length) return errQUEUE_FULL;
  const uint8_t* bytes = static_cast<const uint8_t*>(item);
  queue->items.emplace_back(bytes, bytes + queue->itemSize);
  return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait) {
  (void)wait;
  if (!queue || queue->items.empty()) return pdFALSE;
  std::memcpy(item, queue->items.front().data(), queue->itemSize);
  queue->items.pop_front();
  return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
  return queue ? static_cast<UBaseType_t>(queue->items.size()) : 0;
}

namespace fake {

const std::vector<TaskRecord>& tasks() { return taskRecords(); }

}  // namespace fake
