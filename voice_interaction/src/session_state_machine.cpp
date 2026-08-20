#include "session_state_machine.hpp"

#include <algorithm>
#include <chrono>

SessionStateMachine::SessionStateMachine(
    const rclcpp::Logger& logger,
    const SessionStateMachineConfig& config,
    std::unique_ptr<LLMClient> llm)
    : logger_(logger), config_(config), llm_(std::move(llm)) {
    config_.max_history = std::max(0, config_.max_history);
    config_.conversation_timeout_sec =
        std::max(0.5, config_.conversation_timeout_sec);
    config_.tts_cooldown_ms = std::max(0, config_.tts_cooldown_ms);
    config_.barge_in_hold_ms = std::max(100, config_.barge_in_hold_ms);
    config_.barge_in_guard_ms = std::max(0, config_.barge_in_guard_ms);

    config_.recorder.barge_in_hold_ms = config_.barge_in_hold_ms;
    recorder_ = std::make_unique<SpeechRecorder>(logger_, config_.recorder);
    playback_ = std::make_unique<PlaybackManager>(logger_, config_.playback);
    if (!llm_) llm_ = std::make_unique<LLMClient>();
    response_streamer_ =
        std::make_unique<ResponseStreamer>(logger_, *llm_, *playback_);

    if (!config_.system_prompt.empty())
        history_.push_back({"system", config_.system_prompt});

    if (!is_ready()) return;

    worker_running_.store(true);
    worker_thread_ = std::thread(&SessionStateMachine::worker_loop, this);
}

SessionStateMachine::~SessionStateMachine() {
    shutdown();
}

bool SessionStateMachine::is_ready() const {
    return recorder_ && recorder_->is_ready();
}

void SessionStateMachine::on_audio(const std::vector<int16_t>& samples) {
    if (!is_ready()) return;
    const bool allow_wake =
        state_.load() == SessionState::IDLE &&
        !in_conversation_.load() &&
        now_ms() >= cooldown_until_ms_.load();
    recorder_->on_audio(samples, allow_wake);
}

void SessionStateMachine::on_vad(bool active) {
    if (recorder_) recorder_->on_vad(active);
}

void SessionStateMachine::on_tick() {
    if (!worker_running_.load()) return;

    switch (state_.load()) {
        case SessionState::IDLE:
            tick_idle();
            break;
        case SessionState::LISTENING:
            tick_listening();
            break;
        case SessionState::THINKING:
            break;
        case SessionState::SPEAKING:
            tick_speaking();
            break;
    }
}

void SessionStateMachine::tick_idle() {
    const int64_t now = now_ms();
    if (now < cooldown_until_ms_.load()) return;

    if (in_conversation_.load()) {
        const int64_t last = last_recognized_ms_.load();
        if (last > 0 &&
            now - last >=
                static_cast<int64_t>(config_.conversation_timeout_sec * 1000)) {
            RCLCPP_INFO(logger_, "连续对话超时，回到待唤醒");
            in_conversation_.store(false);
            recorder_->reset_wake_detector();
        }
    }

    if (!in_conversation_.load() && recorder_->wake_word_enabled()) {
        const std::string wake_text = recorder_->poll_wake_word();
        if (!wake_text.empty()) {
            RCLCPP_INFO(logger_, "唤醒词触发: %s", wake_text.c_str());
            in_conversation_.store(true);
            last_recognized_ms_.store(now);
            // Do not speak here. Capture the rest of the same utterance first.
            start_listening(CaptureMode::WakeWord);
        }
        return;
    }

    if (recorder_->vad_active()) {
        RCLCPP_INFO(logger_, "检测到语音，开始录音...");
        start_listening(CaptureMode::VAD);
    }
}

void SessionStateMachine::tick_listening() {
    const auto result = recorder_->tick_listening(now_ms());
    if (result.kind == SpeechRecorder::TickKind::WaitTimeout) {
        RCLCPP_WARN(logger_, "等待用户说话超时，回到待唤醒");
        in_conversation_.store(false);
        invalidate_generation();
        state_.store(SessionState::IDLE);
        cooldown_until_ms_.store(now_ms() + config_.tts_cooldown_ms);
        recorder_->reset_wake_detector();
        return;
    }

    if (result.kind == SpeechRecorder::TickKind::UtteranceReady)
        finish_listening(result);
}

void SessionStateMachine::tick_speaking() {
    const int64_t now = now_ms();
    if (!config_.enable_barge_in ||
        now < barge_in_guard_until_ms_.load()) {
        barge_in_count_ = 0;
        return;
    }

    if (recorder_->vad_active())
        ++barge_in_count_;
    else
        barge_in_count_ = 0;

    const int required =
        std::max(1, (config_.barge_in_hold_ms + 99) / 100);
    if (barge_in_count_ < required) return;

    RCLCPP_INFO(logger_, "检测到用户插话，停止当前回复");
    barge_in_count_ = 0;
    response_streamer_->cancel();
    start_listening(CaptureMode::BargeIn);
}

void SessionStateMachine::start_listening(CaptureMode mode) {
    conversation_generation_.fetch_add(1);
    recorder_->start_listening(mode, now_ms());
    state_.store(SessionState::LISTENING);
}

void SessionStateMachine::finish_listening(
    const SpeechRecorder::TickResult& result) {
    if (result.audio.empty()) {
        state_.store(SessionState::IDLE);
        return;
    }

    const uint64_t generation = conversation_generation_.load();
    state_.store(SessionState::THINKING);
    enqueue_audio({generation, result.audio});
    RCLCPP_INFO(logger_, "说话结束，进入 ASR/LLM 处理");
}

void SessionStateMachine::enqueue_audio(AudioWork work) {
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_queue_.clear();
        work_queue_.push_back(std::move(work));
    }
    work_cv_.notify_one();
}

void SessionStateMachine::worker_loop() {
    while (worker_running_.load()) {
        AudioWork work;
        {
            std::unique_lock<std::mutex> lock(work_mutex_);
            work_cv_.wait(lock, [this]() {
                return !worker_running_.load() || !work_queue_.empty();
            });
            if (!worker_running_.load()) break;
            work = std::move(work_queue_.front());
            work_queue_.pop_front();
        }
        process_audio(work);
    }
}

void SessionStateMachine::process_audio(const AudioWork& work) {
    if (!generation_active(work.generation)) return;

    RCLCPP_INFO(logger_, "ASR... (%zu samples)", work.audio.size());
    const std::string recognized =
        recorder_->transcribe(work.audio, [this, &work]() {
            return generation_active(work.generation);
        });
    if (!generation_active(work.generation)) return;

    const std::string text = recorder_->strip_wake_word(recognized);
    if (text.empty()) {
        RCLCPP_INFO(logger_, "只听到唤醒词，提示用户继续说");
        state_.store(SessionState::SPEAKING);
        barge_in_guard_until_ms_.store(
            now_ms() + config_.barge_in_guard_ms);
        response_streamer_->speak_text(
            "你好，有什么可以帮你吗？",
            [this, generation = work.generation]() {
                return generation_active(generation);
            });
        if (!generation_active(work.generation)) return;
        last_recognized_ms_.store(now_ms());
        cooldown_until_ms_.store(now_ms() + config_.tts_cooldown_ms);
        state_.store(SessionState::IDLE);
        return;
    }

    RCLCPP_INFO(logger_, "识别: %s", text.c_str());
    if (is_goodbye(text)) {
        RCLCPP_INFO(logger_, "识别到告别语，退出连续对话");
        in_conversation_.store(false);
        recorder_->reset_wake_detector();
        state_.store(SessionState::SPEAKING);
        barge_in_guard_until_ms_.store(
            now_ms() + config_.barge_in_guard_ms);
        response_streamer_->speak_text(
            "好的，再见",
            [this, generation = work.generation]() {
                return generation_active(generation);
            });
        if (!generation_active(work.generation)) return;
        cooldown_until_ms_.store(now_ms() + config_.tts_cooldown_ms);
        state_.store(SessionState::IDLE);
        return;
    }

    std::vector<ChatMessage> request_history;
    {
        std::lock_guard<std::mutex> lock(history_mutex_);
        request_history = history_;
        request_history.push_back({"user", text});
    }

    state_.store(SessionState::SPEAKING);
    barge_in_guard_until_ms_.store(now_ms() + config_.barge_in_guard_ms);
    const auto result = response_streamer_->stream(
        request_history,
        [this, generation = work.generation]() {
            return generation_active(generation);
        });
    if (!generation_active(work.generation)) return;

    if (!result.reply.empty()) {
        RCLCPP_INFO(logger_, "回复(%s): %s",
                    result.provider.c_str(), result.reply.c_str());
        std::lock_guard<std::mutex> lock(history_mutex_);
        history_.push_back({"user", text});
        history_.push_back({"assistant", result.reply});
        while (static_cast<int>(history_.size()) > 1 + config_.max_history * 2)
            history_.erase(history_.begin() + 1);
    }

    last_recognized_ms_.store(now_ms());
    cooldown_until_ms_.store(now_ms() + config_.tts_cooldown_ms);
    state_.store(SessionState::IDLE);
    RCLCPP_INFO(logger_, "就绪");
}

bool SessionStateMachine::generation_active(uint64_t generation) const {
    return worker_running_.load() &&
           conversation_generation_.load() == generation;
}

int64_t SessionStateMachine::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void SessionStateMachine::invalidate_generation() {
    conversation_generation_.fetch_add(1);
}

void SessionStateMachine::shutdown() {
    if (!worker_running_.exchange(false)) return;
    invalidate_generation();
    if (response_streamer_) response_streamer_->cancel();
    work_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool SessionStateMachine::is_goodbye(const std::string& text) {
    static const std::vector<std::string> words = {
        "再见", "拜拜", "没事了", "晚安", "下次见"};
    for (const auto& word : words)
        if (text.find(word) != std::string::npos) return true;
    return false;
}
