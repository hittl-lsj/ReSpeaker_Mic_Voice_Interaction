#pragma once

#include <rclcpp/rclcpp.hpp>

#include "asr_sherpa.hpp"
#include "keyword_spotter.hpp"
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
        std::string asr_model_dir;  // 指令 ASR 模型目录
        bool use_wake_word = true;  // 是否必须先命中唤醒词
        std::string wake_detector = "kws";  // kws、asr 或 off
        std::string wake_word;  // 主唤醒词
        std::vector<std::string> wake_word_aliases;  // ASR 同音/繁体别名
        std::string kws_model_dir;  // KWS 模型目录
        std::string kws_encoder;  // 可选：显式 encoder 路径
        std::string kws_decoder;  // 可选：显式 decoder 路径
        std::string kws_joiner;  // 可选：显式 joiner 路径
        std::string kws_tokens;  // 可选：显式 tokens.txt 路径
        std::string kws_keywords_file;  // KWS 关键词文件
        int kws_num_threads = 1;  // KWS 推理线程数
        int kws_max_active_paths = 4;  // KWS 搜索候选路径数
        int kws_num_trailing_blanks = 1;  // 关键词后的确认 blank 数
        float kws_keywords_score = 3.0f;  // 关键词 token 加分
        float kws_keywords_threshold = 0.1f;  // KWS 触发阈值
        int preroll_ms = 500;  // 普通 VAD 触发时保留的预录时长
        int wake_preroll_ms = 2000;  // 唤醒触发时保留的预录时长
        int silence_threshold = 5;  // 连续静音 tick 数，约为 silence_sec * 10
        double wait_user_timeout_sec = 8.0;  // 唤醒后等待用户开口的超时
        double max_utterance_sec = 15.0;  // 单句录音最长时长
        int barge_in_hold_ms = 300;  // 插话确认所需的连续语音时长
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

    /** 创建 ASR、KWS 和录音缓冲；KWS 不可用时按配置回退。 */
    SpeechRecorder(const rclcpp::Logger& logger, const Config& config);

    /** 指令 ASR 是否已经成功加载。 */
    bool is_ready() const;

    /** 是否存在可用的唤醒检测器。 */
    bool wake_word_enabled() const {
        return use_wake_word_ && (kws_ || wake_asr_);
    }

    /** 最近一次 VAD 状态。 */
    bool vad_active() const { return last_vad_.load(); }

    /**
     * @param samples 一批 16 kHz、16-bit、mono PCM
     * @param feed_wake_word 是否同时喂给 KWS/ASR 唤醒检测器
     */
    void on_audio(const std::vector<int16_t>& samples, bool feed_wake_word);

    /** 更新来自 respeaker_driver 的 VAD 状态。 */
    void on_vad(bool active) { last_vad_.store(active); }

    /** 获取并消费 ASR 唤醒检测的匹配文本；没有命中时返回空字符串。 */
    std::string poll_wake_word();
    /** 清空 KWS/ASR 唤醒检测器的内部 stream。 */
    void reset_wake_detector();

    /** 开始一次录音，并按 mode 决定预录音频范围。 */
    void start_listening(CaptureMode mode, int64_t now_ms);

    /** 根据 VAD、静音和超时条件推进当前录音。 */
    TickResult tick_listening(int64_t now_ms);

    /** 取消当前录音并清空音频缓冲。 */
    void abort_listening();

    /**
     * @param audio 已切分的一句话 PCM 音频
     * @param should_continue 返回 false 时中止识别
     * @return 最终识别文本
     */
    std::string transcribe(const std::vector<int16_t>& audio,
                           const std::function<bool()>& should_continue);

    /** 从识别文本开头移除唤醒词及其别名。 */
    std::string strip_wake_word(const std::string& text) const;

private:
    rclcpp::Logger logger_;
    Config config_;

    std::unique_ptr<ASRSherpa> asr_;
    std::unique_ptr<ASRSherpa> wake_asr_;
    std::unique_ptr<KeywordSpotter> kws_;
    std::string wake_detector_;
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
