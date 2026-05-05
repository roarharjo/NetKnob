#include "safe_lock.h"
#include "settings.h"
#include <Arduino.h>
#include <string.h>

static SafeLockState state;

static const uint32_t lockout_delays[] = { 1000, 5000, 30000 };

void safe_lock_init() {
    memset(&state, 0, sizeof(state));
    state.phase = LOCK_DIGIT_1_CW;
    state.mode = LOCKMODE_VERIFY;
}

void safe_lock_reset() {
    state.current_position = 0;
    state.phase = LOCK_DIGIT_1_CW;
    state.mode = LOCKMODE_VERIFY;
    state.digit_confirmed = false;
    state.has_moved = false;
    state.last_encoder_ms = millis();
    memset(state.entered_digits, 0, sizeof(state.entered_digits));
}

void safe_lock_start_set_code() {
    state.current_position = 0;
    state.phase = LOCK_DIGIT_1_CW;
    state.mode = LOCKMODE_SET_NEW;
    state.digit_confirmed = false;
    state.has_moved = false;
    state.last_encoder_ms = millis();
    memset(state.entered_digits, 0, sizeof(state.entered_digits));
    memset(state.new_code, 0, sizeof(state.new_code));
    Serial.println("[lock] set-code mode started");
}

void safe_lock_on_encoder(int8_t delta) {
    if (safe_lock_is_locked_out()) return;
    if (state.phase == LOCK_SUCCESS || state.phase == LOCK_FAILED) return;

    bool cw = (delta > 0);
    bool ccw = (delta < 0);

    switch (state.phase) {
        case LOCK_DIGIT_1_CW:
        case LOCK_DIGIT_3_CW:
            if (!cw) return;
            break;
        case LOCK_DIGIT_2_CCW:
            if (!ccw) return;
            break;
        case LOCK_OPEN_CCW:
            if (ccw) {
                if (state.mode == LOCKMODE_VERIFY) {
                    // Verify against stored code
                    bool ok = settings_verify_lock_code(
                        state.entered_digits[0],
                        state.entered_digits[1],
                        state.entered_digits[2]);
                    state.phase = ok ? LOCK_SUCCESS : LOCK_FAILED;
                    if (!ok) {
                        state.attempt_count++;
                        uint8_t idx = (state.attempt_count - 1);
                        if (idx > 2) idx = 2;
                        state.lockout_until_ms = millis() + lockout_delays[idx];
                    }
                } else if (state.mode == LOCKMODE_SET_NEW) {
                    // First entry done — store and ask for confirmation
                    memcpy(state.new_code, state.entered_digits, 3);
                    state.current_position = 0;
                    state.phase = LOCK_DIGIT_1_CW;
                    state.mode = LOCKMODE_SET_CONFIRM;
                    state.digit_confirmed = false;
                    state.has_moved = false;
                    state.last_encoder_ms = millis();
                    memset(state.entered_digits, 0, sizeof(state.entered_digits));
                    Serial.printf("[lock] new code entered: %d-%d-%d, confirm now\n",
                        state.new_code[0], state.new_code[1], state.new_code[2]);
                } else if (state.mode == LOCKMODE_SET_CONFIRM) {
                    // Check if confirmation matches
                    bool match = (state.entered_digits[0] == state.new_code[0] &&
                                  state.entered_digits[1] == state.new_code[1] &&
                                  state.entered_digits[2] == state.new_code[2]);
                    if (match) {
                        settings_set_lock_code(state.new_code[0], state.new_code[1], state.new_code[2]);
                        state.phase = LOCK_SUCCESS;
                        Serial.printf("[lock] code saved: %d-%d-%d\n",
                            state.new_code[0], state.new_code[1], state.new_code[2]);
                    } else {
                        state.phase = LOCK_FAILED;
                        Serial.println("[lock] confirmation mismatch");
                    }
                }
                return;
            }
            return;
        default:
            return;
    }

    // Move dial
    int8_t abs_delta = (delta > 0) ? delta : -delta;
    for (int8_t i = 0; i < abs_delta; i++) {
        if (cw) {
            state.current_position = (state.current_position + 1) % LOCK_POSITIONS;
        } else {
            state.current_position = (state.current_position + LOCK_POSITIONS - 1) % LOCK_POSITIONS;
        }
    }

    state.last_encoder_ms = millis();
    state.digit_confirmed = false;
    state.has_moved = true;
}

void safe_lock_update() {
    if (state.phase >= LOCK_OPEN_CCW) return;
    if (state.digit_confirmed) return;
    if (!state.has_moved) return;  // Don't auto-confirm until user has moved encoder

    uint32_t now = millis();
    if (state.last_encoder_ms == 0) {
        state.last_encoder_ms = now;
        return;
    }

    if (now - state.last_encoder_ms >= LOCK_STOP_MS) {
        uint8_t digit_idx = 0;
        switch (state.phase) {
            case LOCK_DIGIT_1_CW:  digit_idx = 0; break;
            case LOCK_DIGIT_2_CCW: digit_idx = 1; break;
            case LOCK_DIGIT_3_CW:  digit_idx = 2; break;
            default: return;
        }

        state.entered_digits[digit_idx] = state.current_position;
        state.digit_confirmed = true;
        state.has_moved = false;  // Must move again for next digit

        switch (state.phase) {
            case LOCK_DIGIT_1_CW:  state.phase = LOCK_DIGIT_2_CCW; break;
            case LOCK_DIGIT_2_CCW: state.phase = LOCK_DIGIT_3_CW;  break;
            case LOCK_DIGIT_3_CW:  state.phase = LOCK_OPEN_CCW;    break;
            default: break;
        }
    }
}

LockPhase safe_lock_get_phase() {
    return state.phase;
}

LockMode safe_lock_get_mode() {
    return state.mode;
}

const SafeLockState* safe_lock_get_state() {
    return &state;
}

bool safe_lock_is_locked_out() {
    if (state.attempt_count == 0) return false;
    return millis() < state.lockout_until_ms;
}
