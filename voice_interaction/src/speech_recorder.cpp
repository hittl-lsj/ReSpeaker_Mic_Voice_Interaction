#include "speech_recorder.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>

SpeechRecorder::SpeechRecorder(const rclcpp::Logger& logger, const Config& config)
    : logger_(logger),
      config_(config),
      wake_detector_(config.wake_detector),
      wake_word_(config.wake_word) {
    if (wake_detector_ != "kws" && wake_detector_ != "asr" &&
        wake_detector_ != "off") {
        RCLCPP_WARN(logger_, "未知 wake_detector=%s，使用 kws",
                    wake_detector_.c_str());
        wake_detector_ = "kws";
    }

    config_.preroll_ms = std::max(0, config_.preroll_ms);
    config_.wake_preroll_ms =
        std::max(config_.preroll_ms, config_.wake_preroll_ms);
    config_.silence_threshold = std::max(1, config_.silence_threshold);
    config_.wait_user_timeout_sec = std::max(0.5, config_.wait_user_timeout_sec);
    config_.max_utterance_sec = std::max(1.0, config_.max_utterance_sec);
    config_.barge_in_hold_ms = std::max(100, config_.barge_in_hold_ms);

    asr_ = std::make_unique<ASRSherpa>(config_.asr_model_dir);
    if (!asr_->is_ready()) {
        RCLCPP_ERROR(logger_, "ASR 模型加载失败");
        return;
    }

    wake_word_aliases_ = config_.wake_word_aliases;
    if (wake_word_aliases_.empty()) {
        wake_word_aliases_.push_back(wake_word_);
    } else if (std::find(wake_word_aliases_.begin(), wake_word_aliases_.end(),
                         wake_word_) == wake_word_aliases_.end()) {
        wake_word_aliases_.insert(wake_word_aliases_.begin(), wake_word_);
    }

    use_wake_word_ = config_.use_wake_word &&
                     wake_detector_ != "off";
    if (use_wake_word_ && wake_detector_ == "kws") {
        KeywordSpotter::Config kws_config;
        kws_config.model_dir = config_.kws_model_dir;
        kws_config.encoder = config_.kws_encoder;
        kws_config.decoder = config_.kws_decoder;
        kws_config.joiner = config_.kws_joiner;
        kws_config.tokens = config_.kws_tokens;
        kws_config.keywords_file = config_.kws_keywords_file;
        kws_config.keywords = wake_word_aliases_;
        kws_config.num_threads = config_.kws_num_threads;
        kws_config.max_active_paths = config_.kws_max_active_paths;
        kws_config.num_trailing_blanks = config_.kws_num_trailing_blanks;
        kws_config.keywords_score = config_.kws_keywords_score;
        kws_config.keywords_threshold = config_.kws_keywords_threshold;
        kws_ = std::make_unique<KeywordSpotter>(logger_, kws_config);
        if (!kws_->is_ready()) {
            kws_.reset();
            RCLCPP_WARN(logger_, "KWS 不可用，回退到 ASR 唤醒");
            wake_detector_ = "asr";
        }
    }

    if (use_wake_word_ && wake_detector_ == "asr") {
        wake_asr_ = std::make_unique<ASRSherpa>(config_.asr_model_dir);
        if (!wake_asr_->is_ready()) {
            RCLCPP_WARN(logger_, "唤醒词识别器加载失败，回退到 VAD 触发");
            wake_asr_.reset();
            use_wake_word_ = false;
        } else {
            RCLCPP_INFO(logger_, "ASR 唤醒已启用: %s", wake_word_.c_str());
        }
    }

}

bool SpeechRecorder::is_ready() const {
    return asr_ && asr_->is_ready();
}

void SpeechRecorder::on_audio(const std::vector<int16_t>& samples,
                              bool feed_wake_word) {
    if (samples.empty()) return;

    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        preroll_buffer_.insert(preroll_buffer_.end(), samples.begin(), samples.end());

        const size_t max_preroll =
            static_cast<size_t>(std::max(config_.preroll_ms,
                                         config_.wake_preroll_ms)) * 16;
        while (preroll_buffer_.size() > max_preroll)
            preroll_buffer_.pop_front();

        if (listening_)
            audio_buffer_.insert(audio_buffer_.end(), samples.begin(), samples.end());
    }

    if (!feed_wake_word || !use_wake_word_) return;
    if (kws_) {
        kws_->feed(samples);
    } else if (wake_asr_) {
        wake_asr_->feed(samples);
    }
}

std::string SpeechRecorder::poll_wake_word() {
    if (!use_wake_word_) return "";
    if (kws_) return kws_->poll();
    if (!wake_asr_) return "";

    const std::string partial = wake_asr_->partial_result();
    if (!partial.empty() && wake_word_matches(partial)) return partial;
    return "";
}

void SpeechRecorder::reset_wake_detector() {
    if (kws_) kws_->reset();
    if (wake_asr_) wake_asr_->reset();
}

void SpeechRecorder::start_listening(CaptureMode mode, int64_t now_ms) {
    capture_mode_ = mode;
    listening_ = true;
    silence_count_ = 0;
    listening_started_ms_ = now_ms;
    speech_started_ms_ = now_ms;

    // WakeWord mode deliberately starts immediately and keeps the pre-roll.
    // This is what preserves "小萝卜头，打开..." as one utterance.
    heard_speech_ = true;

    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_buffer_.clear();
        const int preroll_ms =
            mode == CaptureMode::WakeWord ? config_.wake_preroll_ms
                                          : config_.preroll_ms;
        size_t keep = std::min(
            preroll_buffer_.size(), static_cast<size_t>(preroll_ms) * 16);
        if (mode == CaptureMode::BargeIn) {
            const size_t barge_preroll =
                static_cast<size_t>(config_.barge_in_hold_ms + 100) * 16;
            keep = std::min(keep, barge_preroll);
        }
        if (keep > 0) {
            audio_buffer_.insert(audio_buffer_.end(),
                                 preroll_buffer_.end() - keep,
                                 preroll_buffer_.end());
        }
    }
    reset_wake_detector();
}

SpeechRecorder::TickResult SpeechRecorder::tick_listening(int64_t now_ms) {
    if (!listening_) return {};

    if (last_vad_.load()) {
        heard_speech_ = true;
        speech_started_ms_ = speech_started_ms_ > 0 ? speech_started_ms_ : now_ms;
        silence_count_ = 0;
    } else if (heard_speech_) {
        ++silence_count_;
    }

    if (!heard_speech_ &&
        now_ms - listening_started_ms_ >=
            static_cast<int64_t>(config_.wait_user_timeout_sec * 1000)) {
        listening_ = false;
        return {TickKind::WaitTimeout, {}};
    }

    if (heard_speech_ && speech_started_ms_ > 0 &&
        now_ms - speech_started_ms_ >=
            static_cast<int64_t>(config_.max_utterance_sec * 1000)) {
        RCLCPP_INFO(logger_, "达到最大录音时长，强制结束本句");
        listening_ = false;
        return {TickKind::UtteranceReady, take_audio()};
    }

    if (silence_count_ >= config_.silence_threshold) {
        listening_ = false;
        return {TickKind::UtteranceReady, take_audio()};
    }
    return {};
}

void SpeechRecorder::abort_listening() {
    listening_ = false;
    silence_count_ = 0;
    heard_speech_ = false;
    std::lock_guard<std::mutex> lock(audio_mutex_);
    audio_buffer_.clear();
}

std::string SpeechRecorder::transcribe(
    const std::vector<int16_t>& audio,
    const std::function<bool()>& should_continue) {
    if (!is_ready() || audio.empty()) return "";

    static constexpr size_t kChunk = 512;
    for (size_t i = 0; i < audio.size(); i += kChunk) {
        if (should_continue && !should_continue()) {
            asr_->reset();
            return "";
        }
        const size_t n = std::min(audio.size() - i, kChunk);
        asr_->feed(std::vector<int16_t>(audio.begin() + i,
                                        audio.begin() + i + n));
    }

    std::string text = asr_->final_result();
    asr_->reset();
    return text;
}

std::string SpeechRecorder::strip_wake_word(const std::string& text) const {
    if (!use_wake_word_ || wake_word_.empty()) return text;

    const std::string normalized = normalize_wake_text(text);
    for (const auto& alias : wake_word_aliases_) {
        const std::string candidate = normalize_wake_text(alias);
        if (candidate.empty()) continue;
        if (normalized.rfind(candidate, 0) == 0)
            return normalized.substr(candidate.size());
    }
    return text;
}

void SpeechRecorder::replace_all(std::string& text, const std::string& from,
                                 const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string SpeechRecorder::normalize_wake_text(std::string text) {
    for (auto it = text.begin(); it != text.end();) {
        const unsigned char c = static_cast<unsigned char>(*it);
        if (c < 0x80 && (std::isspace(c) || std::ispunct(c)))
            it = text.erase(it);
        else
            ++it;
    }
    replace_all(text, "罗", "萝");
    replace_all(text, "羅", "萝");
    replace_all(text, "布", "卜");
    replace_all(text, "頭", "头");
    replace_all(text, "。", "");
    replace_all(text, "，", "");
    replace_all(text, "！", "");
    replace_all(text, "？", "");
    return text;
}

bool SpeechRecorder::wake_word_matches(const std::string& text) const {
    const std::string normalized = normalize_wake_text(text);
    for (const auto& alias : wake_word_aliases_) {
        const std::string candidate = normalize_wake_text(alias);
        if (!candidate.empty() && normalized.find(candidate) != std::string::npos)
            return true;
    }
    return false;
}

std::vector<int16_t> SpeechRecorder::take_audio() {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    std::vector<int16_t> audio(audio_buffer_.begin(), audio_buffer_.end());
    audio_buffer_.clear();
    return audio;
}
