#pragma once

#include <stdint.h>

#define LOCK_POSITIONS 40
#define LOCK_DIGITS 3
#define LOCK_STOP_MS 500       // No encoder activity for this long = confirm digit

enum LockPhase {
    LOCK_DIGIT_1_CW,      // Rotating CW to first digit
    LOCK_DIGIT_2_CCW,     // Rotating CCW to second digit
    LOCK_DIGIT_3_CW,      // Rotating CW to third digit
    LOCK_OPEN_CCW,        // Short CCW to confirm/open
    LOCK_SUCCESS,
    LOCK_FAILED
};

enum LockMode {
    LOCKMODE_VERIFY,      // Normal unlock flow
    LOCKMODE_SET_NEW,     // Entering new code (first time)
    LOCKMODE_SET_CONFIRM  // Confirming new code (second time)
};

struct SafeLockState {
    uint8_t   current_position;    // 0-39
    uint8_t   entered_digits[3];
    LockPhase phase;
    LockMode  mode;
    uint8_t   attempt_count;       // For escalating lockout
    uint32_t  lockout_until_ms;    // millis() when lockout ends
    uint32_t  last_encoder_ms;     // For stop detection
    bool      digit_confirmed;     // Set when stop detected
    bool      has_moved;           // True after first encoder input in current phase
    uint8_t   new_code[3];         // Stored during SET_NEW, compared in SET_CONFIRM
};

void safe_lock_init();
void safe_lock_reset();            // Reset to digit 1 (verify mode)
void safe_lock_start_set_code();   // Enter set-code mode
void safe_lock_on_encoder(int8_t delta);
void safe_lock_update();           // Call from screen update — handles stop detection
LockPhase safe_lock_get_phase();
LockMode safe_lock_get_mode();
const SafeLockState* safe_lock_get_state();
bool safe_lock_is_locked_out();
