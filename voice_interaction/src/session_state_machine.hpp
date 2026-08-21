#pragma once

#include <rclcpp/rclcpp.hpp>

#include "llm_client.hpp"
#include "playback_manager.hpp"
#include "response_streamer.hpp"
#include "speech_recorder.hpp"
#include "voice_types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct SessionStateMachineConfig {
    SpeechRecorder::Config recorder;  // 录音、VAD、唤醒和 ASR 配置
    PlaybackManager::Config playback;  // TTS 与音频输出配置
    std::string system_prompt;  // 发给 LLM 的系统提示词
    int max_history = 6;  // 保留的 user/assistant 消息轮数
    double conversation_timeout_sec = 20.0;  // 连续对话空闲超时
    int tts_cooldown_ms = 500;  // 播放结束后的 VAD 冷却时间
    bool enable_barge_in = true;  // 是否允许用户打断机器人
    int barge_in_hold_ms = 300;  // 连续语音多久后确认插话
    int barge_in_guard_ms = 500;  // TTS 开始后暂不检测插话的保护时间
};

class SessionStateMachine {
public:
    /**
     * @param logger ROS 日志对象
     * @param config 会话及其子模块配置
     * @param llm 已配置 provider 的 LLM 客户端
     */
    SessionStateMachine(const rclcpp::Logger& logger,
                        const SessionStateMachineConfig& config,
                        std::unique_ptr<LLMClient> llm);
    ~SessionStateMachine();

    /** ASR、LLM 和播放组件是否都已满足运行条件。 */
    bool is_ready() const;

    /** 接收 ROS 音频 topic 的 PCM 数据。 */
    void on_audio(const std::vector<int16_t>& samples);

    /** 接收 ROS VAD topic 的状态。 */
    void on_vad(bool active);

    /** 由 ROS 定时器周期性调用，推进状态机。 */
    void on_tick();

    /** 取消当前 generation、停止线程和播放并释放会话资源。 */
    void shutdown();

private:
    struct AudioWork {
        uint64_t generation = 0;
        std::vector<int16_t> audio;
    };

    rclcpp::Logger logger_;
    SessionStateMachineConfig config_;
    std::unique_ptr<LLMClient> llm_;
    std::unique_ptr<SpeechRecorder> recorder_;
    std::unique_ptr<PlaybackManager> playback_;
    std::unique_ptr<ResponseStreamer> response_streamer_;

    std::atomic<SessionState> state_{SessionState::IDLE};
    std::atomic<bool> in_conversation_{false};
    std::atomic<int64_t> last_recognized_ms_{0};
    std::atomic<int64_t> cooldown_until_ms_{0};
    std::atomic<int64_t> barge_in_guard_until_ms_{0};
    std::atomic<uint64_t> conversation_generation_{0};
    std::atomic<bool> worker_running_{false};

    int barge_in_count_ = 0;
    std::thread worker_thread_;
    std::mutex work_mutex_;
    std::condition_variable work_cv_;
    std::deque<AudioWork> work_queue_;

    std::mutex history_mutex_;
    std::vector<ChatMessage> history_;

    void worker_loop();
    void process_audio(const AudioWork& work);
    void tick_idle();
    void tick_listening();
    void tick_speaking();
    void start_listening(CaptureMode mode);
    void finish_listening(const SpeechRecorder::TickResult& result);
    void enqueue_audio(AudioWork work);

    bool generation_active(uint64_t generation) const;
    int64_t now_ms() const;
    void invalidate_generation();
    static bool is_goodbye(const std::string& text);
};
