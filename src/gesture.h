#pragma once

#include <stdint.h>

// Tunable constants
#define BACKSPIN_MIN_VELOCITY    20    // steps/sec CCW threshold
#define BACKSPIN_QUIET_MS        100   // silence after burst confirms intent
#define BACKSPIN_MIN_STEPS       3     // minimum CCW steps in burst
#define SHAKE_REVERSALS          3     // direction changes for emergency stop
#define SHAKE_WINDOW_MS          500   // time window for counting reversals
#define VELOCITY_RING_SIZE       4     // samples for velocity smoothing

enum GestureEvent {
    GESTURE_NONE,
    GESTURE_BACKSPIN,     // Fast CCW flick confirmed
    GESTURE_SHAKE         // 3+ rapid reversals confirmed
};

struct GestureState {
    float     velocity;             // Current velocity (steps/sec), signed (neg=CCW)
    float     peak_velocity;        // Peak in current burst
    uint8_t   ccw_burst_count;      // Consecutive fast CCW steps
    bool      backspin_armed;       // Fast CCW detected, waiting for quiet
    uint32_t  backspin_quiet_start; // millis() when burst ended
    int8_t    last_direction;       // +1 CW, -1 CCW
    uint8_t   reversal_count;       // Reversals in window
    uint32_t  reversal_timestamps[SHAKE_REVERSALS + 1];
};

void gesture_init();
GestureEvent gesture_update();  // Call from main loop; returns detected gesture
const GestureState* gesture_get_state();
int8_t gesture_get_delta();     // Accumulated delta (consumed by caller)
