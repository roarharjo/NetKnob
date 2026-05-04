#include "encoder.h"
#include <Arduino.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/portmacro.h"

#define ENCODER_TICK_US 3000  // 3ms polling interval
#define ENCODER_DEBOUNCE 2    // 2 ticks = 6ms effective debounce

static uint8_t pin_a, pin_b;
static uint8_t level_a, level_b;
static uint8_t debounce_a, debounce_b;
static volatile int8_t delta;
static esp_timer_handle_t timer_handle;

static void process_channel(uint8_t current, uint8_t *prev, uint8_t *cnt, int8_t step) {
    if (current == 0) {
        if (current != *prev)
            *cnt = 0;
        else
            (*cnt)++;
    } else {
        if (current != *prev && ++(*cnt) >= ENCODER_DEBOUNCE) {
            *cnt = 0;
            delta += step;
        } else if (current == *prev) {
            *cnt = 0;
        }
    }
    *prev = current;
}

static void encoder_timer_cb(void *arg) {
    uint8_t a = gpio_get_level((gpio_num_t)pin_a);
    uint8_t b = gpio_get_level((gpio_num_t)pin_b);
    process_channel(a, &level_a, &debounce_a, +1);
    process_channel(b, &level_b, &debounce_b, -1);
}

void encoder_init(uint8_t a, uint8_t b) {
    pin_a = a;
    pin_b = b;
    delta = 0;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << a) | (1ULL << b),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    level_a = gpio_get_level((gpio_num_t)a);
    level_b = gpio_get_level((gpio_num_t)b);
    debounce_a = 0;
    debounce_b = 0;

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = encoder_timer_cb;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "encoder";
    esp_timer_create(&timer_args, &timer_handle);
    esp_timer_start_periodic(timer_handle, ENCODER_TICK_US);

    Serial.printf("[encoder] init on GPIO %d/%d, timer %dus\n", a, b, ENCODER_TICK_US);
}

static portMUX_TYPE delta_mux = portMUX_INITIALIZER_UNLOCKED;

int8_t encoder_get_delta() {
    portENTER_CRITICAL(&delta_mux);
    int8_t d = delta;
    delta = 0;
    portEXIT_CRITICAL(&delta_mux);
    return d;
}
