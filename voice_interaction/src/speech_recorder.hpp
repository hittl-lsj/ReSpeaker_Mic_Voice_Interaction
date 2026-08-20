#pragma once

#include <rclcpp/rclcpp.hpp>

#include "asr_sherpa.hpp"
#include "voice_types.hpp"

#include <atomic>
#include <deque>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class SpeechRecorder {
public:
    struct Config {
        std::string asr_model_dir;
        bool use_wake_word = true;
        std::string wake_word;
        std::vector<std::string> wake_word_aliases;
        int preroll_ms = 500;
        int wake_preroll_ms = 2000;
        int silence_threshold = 5;
        double wait_user_timeout_sec = 8.0;
        double max_utterance_sec = 15.0;
        int barge_in_hold_ms = 300;
    };

    enum class TickKind {
        None,
        WaitTimeout,
        UtteranceReady,
    };

    struct TickResult {
        TickKind kind = TickKind::None;
        std::vector<int16_t> audio;
    };

    SpeechRecorder(const rclcpp::Logger& logger, const Config& config);

    bool is_ready() const;
    bool wake_word_enabled() const { return use_wake_word_; }
    bool vad_active() const { return last_vad_.load(); }

    void on_audio(const std::vector<int16_t>& samples, bool feed_wake_word);
    void on_vad(bool active) { last_vad_.store(active); }

    // Returns the matching partial text, or an empty string when there is no match.
    std::string poll_wake_word();
    void reset_wake_detector();

    void start_listening(CaptureMode mode, int64_t now_ms);
    TickResult tick_listening(int64_t now_ms);
    void abort_listening();

    std::string transcribe(const std::vector<int16_t>& audio,
                           const std::function<bool()>& should_continue);
    std::string strip_wake_word(const std::string& text) const;

private:
    rclcpp::Logger logger_;
    Config config_;

    std::unique_ptr<ASRSherpa> asr_;
    std::unique_ptr<ASRSherpa> wake_asr_;
    bool use_wake_word_ = false;
    std::string wake_word_;
    std::vector<std::string> wake_word_aliases_;

    std::deque<int16_t> audio_buffer_;
    std::deque<int16_t> preroll_buffer_;
    std::mutex audio_mutex_;

    std::atomic<bool> last_vad_{false};
    bool listening_ = false;
    bool heard_speech_ = false;
    int silence_count_ = 0;
    int64_t listening_started_ms_ = 0;
    int64_t speech_started_ms_ = 0;
    CaptureMode capture_mode_ = CaptureMode::VAD;

    static void replace_all(std::string& text, const std::string& from,
                            const std::string& to);
    static std::string normalize_wake_text(std::string text);
    bool wake_word_matches(const std::string& text) const;
    std::vector<int16_t> take_audio();
};
