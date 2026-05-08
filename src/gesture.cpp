#include "gesture.h"
#include "encoder.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

static GestureState state;
static int8_t accumulated_delta = 0;

// Ring buffer for inter-pulse timing
static uint32_t pulse_times[VELOCITY_RING_SIZE];
static uint8_t pulse_idx = 0;
static uint8_t pulse_count = 0;

void gesture_init() {
    memset(&state, 0, sizeof(state));
    memset(pulse_times, 0, sizeof(pulse_times));
    pulse_idx = 0;
    pulse_count = 0;
    accumulated_delta = 0;
}

GestureEvent gesture_update() {
    // Drain encoder events
    EncoderEvent events[ENCODER_EVENT_SLOTS];
    uint8_t n = encoder_get_events(events, ENCODER_EVENT_SLOTS);

    for (uint8_t i = 0; i < n; i++) {
        accumulated_delta += events[i].direction;

        // Velocity calculation from timestamps
        pulse_times[pulse_idx] = events[i].timestamp_us;
        pulse_idx = (pulse_idx + 1) % VELOCITY_RING_SIZE;
        if (pulse_count < VELOCITY_RING_SIZE) pulse_count++;

        if (pulse_count >= 2) {
            uint8_t oldest = (pulse_idx + VELOCITY_RING_SIZE - pulse_count) % VELOCITY_RING_SIZE;
            uint8_t newest = (pulse_idx + VELOCITY_RING_SIZE - 1) % VELOCITY_RING_SIZE;
            uint32_t dt = pulse_times[newest] - pulse_times[oldest];
            if (dt > 0) {
                state.velocity = (float)(pulse_count - 1) * 1000000.0f / (float)dt;
                if (events[i].direction < 0) state.velocity = -state.velocity;
            }
        }

        // --- Shake detection: track direction reversals ---
        if (state.last_direction != 0 && events[i].direction != state.last_direction) {
            uint32_t now_ms = events[i].timestamp_us / 1000;

            if (state.reversal_count < SHAKE_REVERSALS + 1) {
                state.reversal_timestamps[state.reversal_count] = now_ms;
                state.reversal_count++;
            } else {
                for (uint8_t j = 0; j < SHAKE_REVERSALS; j++)
                    state.reversal_timestamps[j] = state.reversal_timestamps[j + 1];
                state.reversal_timestamps[SHAKE_REVERSALS] = now_ms;
            }

            if (state.reversal_count >= SHAKE_REVERSALS) {
                uint32_t first = state.reversal_timestamps[state.reversal_count - SHAKE_REVERSALS];
                if (now_ms - first <= SHAKE_WINDOW_MS) {
                    state.reversal_count = 0;
                    state.backspin_armed = false;
                    state.ccw_burst_count = 0;
                    accumulated_delta = 0;
                    return GESTURE_SHAKE;
                }
            }
        }
        state.last_direction = events[i].direction;

        // --- Backspin detection: fast CCW burst followed by quiet ---
        if (events[i].direction == -1 && state.velocity < -BACKSPIN_MIN_VELOCITY) {
            state.ccw_burst_count++;
            state.backspin_armed = false;  // Still in burst
            if (-state.velocity > state.peak_velocity)
                state.peak_velocity = -state.velocity;
        } else if (events[i].direction == +1) {
            state.ccw_burst_count = 0;
            state.backspin_armed = false;
            state.peak_velocity = 0;
        }
    }

    // --- Backspin quiet period check (runs every loop, not per-event) ---
    uint32_t now = millis();

    if (!state.backspin_armed && state.ccw_burst_count >= BACKSPIN_MIN_STEPS) {
        if (n == 0) {
            state.backspin_armed = true;
            state.backspin_quiet_start = now;
        }
    }

    if (state.backspin_armed) {
        if (now - state.backspin_quiet_start >= BACKSPIN_QUIET_MS) {
            state.backspin_armed = false;
            state.ccw_burst_count = 0;
            state.peak_velocity = 0;
            accumulated_delta = 0;
            return GESTURE_BACKSPIN;
        }
    }

    // Decay velocity when no events for a while
    if (n == 0 && pulse_count > 0) {
        uint32_t last = pulse_times[(pulse_idx + VELOCITY_RING_SIZE - 1) % VELOCITY_RING_SIZE];
        if ((uint32_t)esp_timer_get_time() - last > 200000) {
            state.velocity = 0;
            pulse_count = 0;
        }
    }

    return GESTURE_NONE;
}

int8_t gesture_get_delta() {
    int8_t d = accumulated_delta;
    accumulated_delta = 0;
    return d;
}

const GestureState* gesture_get_state() {
    return &state;
}
