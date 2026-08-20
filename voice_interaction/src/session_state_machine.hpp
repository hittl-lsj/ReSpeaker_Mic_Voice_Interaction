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
    SpeechRecorder::Config recorder;
    PlaybackManager::Config playback;
    std::string system_prompt;
    int max_history = 6;
    double conversation_timeout_sec = 20.0;
    int tts_cooldown_ms = 500;
    bool enable_barge_in = true;
    int barge_in_hold_ms = 300;
    int barge_in_guard_ms = 500;
};

class SessionStateMachine {
public:
    SessionStateMachine(const rclcpp::Logger& logger,
                        const SessionStateMachineConfig& config,
                        std::unique_ptr<LLMClient> llm);
    ~SessionStateMachine();

    bool is_ready() const;
    void on_audio(const std::vector<int16_t>& samples);
    void on_vad(bool active);
    void on_tick();
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
