#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"

#define PING_PONG_ROUNDS 10
#define QUEUE_LENGTH     1
#define ITEM_SIZE        sizeof(int)

static const char *TAG = "pingpong";

static QueueHandle_t ping_to_pong_queue;
static QueueHandle_t pong_to_ping_queue;

static void ping_task(void *arg)
{
    int counter = 0;
    for (int i = 0; i < PING_PONG_ROUNDS; i++) {
        ESP_LOGI(TAG, "Ping sends: %d", counter);
        xQueueSend(ping_to_pong_queue, &counter, portMAX_DELAY);

        int received;
        xQueueReceive(pong_to_ping_queue, &received, portMAX_DELAY);
        ESP_LOGI(TAG, "Ping received back: %d", received);

        counter = received + 1;
    }
    ESP_LOGI(TAG, "Ping done after %d rounds", PING_PONG_ROUNDS);
    vTaskDelete(NULL);
}

static void pong_task(void *arg)
{
    for (int i = 0; i < PING_PONG_ROUNDS; i++) {
        int received;
        xQueueReceive(ping_to_pong_queue, &received, portMAX_DELAY);
        ESP_LOGI(TAG, "Pong received: %d, sending back: %d", received, received + 1);

        int reply = received + 1;
        xQueueSend(pong_to_ping_queue, &reply, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "Pong done. Exiting.");
    esp_restart();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Creating queues and tasks...");

    ping_to_pong_queue = xQueueCreate(QUEUE_LENGTH, ITEM_SIZE);
    pong_to_ping_queue = xQueueCreate(QUEUE_LENGTH, ITEM_SIZE);
    assert(ping_to_pong_queue != NULL);
    assert(pong_to_ping_queue != NULL);

    xTaskCreate(ping_task, "ping", 4096, NULL, 5, NULL);
    xTaskCreate(pong_task, "pong", 4096, NULL, 5, NULL);
}
