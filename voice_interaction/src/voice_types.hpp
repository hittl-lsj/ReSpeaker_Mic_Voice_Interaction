#pragma once

#include <cstdint>

enum class SessionState {
    IDLE,
    LISTENING,
    THINKING,
    SPEAKING,
};

enum class CaptureMode {
    WakeWord,
    VAD,
    BargeIn,
};
