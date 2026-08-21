#pragma once

#include <cstdint>

/** 会话主状态；状态转换由 SessionStateMachine 统一管理。 */
enum class SessionState {
    IDLE,
    LISTENING,
    THINKING,
    SPEAKING,
};

/** 当前一次录音的触发来源。 */
enum class CaptureMode {
    WakeWord,
    VAD,
    BargeIn,
};
