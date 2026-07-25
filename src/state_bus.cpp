#include "state_bus.h"

QueueHandle_t cmdQueueHandle = nullptr;
QueueHandle_t stateQueueHandle = nullptr;
volatile bool g_mqttConnected = false;

void state_bus_init() {
  cmdQueueHandle = xQueueCreate(CMD_QUEUE_LEN, sizeof(Command));
  stateQueueHandle = xQueueCreate(STATE_QUEUE_LEN, sizeof(StateUpdate));
}
