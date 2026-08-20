#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/int16_multi_array.hpp>
#include <voice_interfaces/srv/set_led.hpp>

#include "asr_sherpa.hpp"
#include "llm_client.hpp"
#include "tts_edge.hpp"
#include "tts_piper.hpp"
#include "utils.hpp"

#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <utility>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

enum class State { IDLE, LISTENING, THINKING, SPEAKING };

class InteractionNode : public rclcpp::Node {
public:
    InteractionNode() : Node("voice_interaction_node") {
        // 模型默认路径用 $HOME 拼接，适配不同机器（开发机 /home/lsj、X5 /home/sunrise）
        std::string home = std::getenv("HOME") ? std::getenv("HOME") : std::string("/home/lsj");
        std::string asr_model = declare_parameter("asr_model_dir",
            home + "/ReSpearMicArray/models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20");
        std::string system_prompt = declare_parameter("system_prompt", "你好你是一个语音助手。");
        double silence_sec = declare_parameter("silence_sec", 1.0);
        preroll_ms_    = declare_parameter("preroll_ms", 500);
        max_history_   = declare_parameter("max_history", 6);
        wait_user_timeout_sec_ = declare_parameter("wait_user_timeout_sec", 8.0);
        max_utterance_sec_ = declare_parameter("max_utterance_sec", 15.0);
        tts_cooldown_ms_ = declare_parameter("tts_cooldown_ms", 500);
        enable_barge_in_ = declare_parameter("enable_barge_in", true);
        barge_in_hold_ms_ = declare_parameter("barge_in_hold_ms", 300);
        barge_in_guard_ms_ = declare_parameter("barge_in_guard_ms", 500);

        preroll_ms_ = std::max(0, preroll_ms_);
        max_history_ = std::max(0, max_history_);
        wait_user_timeout_sec_ = std::max(0.5, wait_user_timeout_sec_);
        max_utterance_sec_ = std::max(1.0, max_utterance_sec_);
        tts_cooldown_ms_ = std::max(0, tts_cooldown_ms_);
        barge_in_hold_ms_ = std::max(100, barge_in_hold_ms_);
        barge_in_guard_ms_ = std::max(0, barge_in_guard_ms_);

        asr_ = std::make_unique<ASRSherpa>(asr_model);
        if (!asr_->is_ready()) {
            RCLCPP_ERROR(get_logger(), "ASR 模型加载失败！");
            return;  // 交给 main() 判断 is_ready() 干净退出，避免在此 shutdown 后 spin 崩溃
        }
        RCLCPP_INFO(get_logger(), "ASR 模型已加载");

        // ----- 唤醒词识别器（复用同一模型，独立 recognizer，避免与转录线程竞争）-----
        use_wake_word_ = declare_parameter("use_wake_word", true);
        wake_word_     = declare_parameter("wake_word", "你好");
        conversation_timeout_sec_ = declare_parameter("conversation_timeout_sec", 20.0);
        if (use_wake_word_) {
            wake_asr_ = std::make_unique<ASRSherpa>(asr_model);
            if (!wake_asr_->is_ready()) {
                RCLCPP_WARN(get_logger(), "唤醒词识别器加载失败，回退到 VAD 触发");
                use_wake_word_ = false;
                wake_asr_.reset();
            } else {
                RCLCPP_INFO(get_logger(), "唤醒词已启用: %s", wake_word_.c_str());
            }
        }

        // ----- LLM providers -----

        // HTTP 代理：开发机走本地代理，部署环境（X5）留空直连
        std::string http_proxy = declare_parameter("http_proxy", "");
        if (!http_proxy.empty()) {
            llm_.set_proxy(http_proxy);
            RCLCPP_INFO(get_logger(), "已设置 HTTP 代理: %s", http_proxy.c_str());
        }
        // 网关（云端 LLM，优先级最高）。密钥只从环境变量读取，避免进入配置文件。
        const char* gw_key_env = std::getenv("VOICE_GATEWAY_API_KEY");
        std::string gw_key = gw_key_env ? gw_key_env : "";
        std::string gw_url = declare_parameter("gateway_base_url", "");
        std::string gw_model = declare_parameter("gateway_model", "");

        if (!gw_key.empty() && !gw_url.empty() && !gw_model.empty()) {
            // 网关仅支持 OpenAI 兼容格式，不支持 Anthropic Messages API
            llm_.add_provider({"网关(OpenAI)", "openai", gw_url, gw_key, gw_model});
            RCLCPP_INFO(get_logger(), "已注册网关");
        } else if (!gw_url.empty() || !gw_model.empty() || !gw_key.empty()) {
            RCLCPP_WARN(get_logger(),
                        "网关配置不完整，需同时设置 gateway_base_url、gateway_model 和 VOICE_GATEWAY_API_KEY");
        }

        // Ollama 本地（可配置开关，X5 上跑 qwen 小模型）
        bool use_ollama = declare_parameter("use_ollama", true);
        if (use_ollama) {
            std::string ollama_url = declare_parameter("ollama_url",
                "http://localhost:11434/v1");
            std::string ollama_model = declare_parameter("ollama_model",
                "qwen2.5:1.5b");
            llm_.add_provider({"Ollama", "openai", ollama_url, "ollama", ollama_model});
            RCLCPP_INFO(get_logger(), "已注册 Ollama 本地: %s @ %s",
                        ollama_model.c_str(), ollama_url.c_str());
        } else {
            RCLCPP_INFO(get_logger(), "Ollama 已禁用（use_ollama=false）");
        }

        if (!system_prompt.empty())
            history_.push_back({"system", system_prompt});

        tts_device_ = declare_parameter("tts_device", "plughw:3,0");
        std::string tts_voice = declare_parameter("tts_voice", "zh-CN-XiaoxiaoNeural");
        tts_edge_ = std::make_unique<TTSEdge>(tts_voice, tts_device_);

        // 离线 fallback：piper（纯 CPU、毫秒级合成）
        std::string piper_model = declare_parameter("piper_model",
            home + "/ReSpearMicArray/zh_CN-huayan-medium.onnx");
        tts_piper_ = std::make_unique<TTSPiper>(piper_model, tts_device_);

        sub_vad_   = create_subscription<std_msgs::msg::Bool>("vad", 10,
                        std::bind(&InteractionNode::on_vad, this, std::placeholders::_1));
        sub_audio_ = create_subscription<std_msgs::msg::Int16MultiArray>("audio_raw", 10,
                        std::bind(&InteractionNode::on_audio, this, std::placeholders::_1));

        led_client_ = create_client<voice_interfaces::srv::SetLED>("set_led");

        timer_ = create_wall_timer(std::chrono::milliseconds(100),
                                   std::bind(&InteractionNode::on_tick, this));

        silence_threshold_ = std::max(1, static_cast<int>(silence_sec * 10));
        state_.store(State::IDLE);
        worker_thread_ = std::thread(&InteractionNode::worker_loop, this);

        RCLCPP_INFO(get_logger(), "语音交互节点启动完成，等待说话...");
    }

    ~InteractionNode() override {
        worker_running_.store(false);
        conversation_generation_.fetch_add(1);
        llm_.cancel_current();
        stop_all_playback();
        work_cv_.notify_all();
        if (worker_thread_.joinable()) worker_thread_.join();
    }

    /** 初始化是否成功（模型加载失败时返回 false） */
    bool is_ready() const { return asr_ && asr_->is_ready(); }

private:
    std::atomic<State> state_{State::IDLE};
    std::unique_ptr<ASRSherpa> asr_;
    std::unique_ptr<ASRSherpa> wake_asr_;    // 唤醒词专用识别器（独立，避免与转录线程竞争）
    bool                     use_wake_word_ = true;
    std::string              wake_word_;
    std::atomic<bool>        in_conversation_{false};    // 唤醒后是否处于连续对话模式
    std::atomic<int64_t>     last_recognized_ms_{0};     // 上次成功识别的时间（超时判定用）
    double                   conversation_timeout_sec_ = 20.0;
    LLMClient                llm_;
    std::unique_ptr<TTSEdge>   tts_edge_;    // 主：edge-tts（联网）
    std::unique_ptr<TTSPiper>  tts_piper_;   // 备：piper（离线 CPU）
    std::vector<ChatMessage> history_;
    std::mutex               history_mutex_;
    int                      max_history_;

    std::thread worker_thread_;
    std::atomic<bool> worker_running_{true};
    std::mutex work_mutex_;
    std::condition_variable work_cv_;
    std::deque<std::pair<uint64_t, std::vector<int16_t>>> work_queue_;
    std::atomic<uint64_t> conversation_generation_{0};

    rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr             sub_vad_;
    rclcpp::Subscription<std_msgs::msg::Int16MultiArray>::SharedPtr  sub_audio_;
    rclcpp::Client<voice_interfaces::srv::SetLED>::SharedPtr         led_client_;
    rclcpp::TimerBase::SharedPtr                                     timer_;

    // 音频缓冲
    std::deque<int16_t> audio_buffer_;
    std::deque<int16_t> preroll_buffer_;  // 预录：IDLE 时持续存，VAD 触发时补到开头
    std::mutex          audio_mutex_;
    int                 silence_count_ = 0;
    int                 silence_threshold_ = 5;
    bool                heard_speech_ = false;   // 进入 LISTENING 后是否已听到语音（避免唤醒应答期间的静音误判）
    int                 preroll_ms_ = 500;
    std::atomic<bool>    last_vad_{false};
    int64_t             listening_started_ms_ = 0;
    int64_t             speech_started_ms_ = 0;
    std::atomic<int64_t> cooldown_until_ms_{0};
    double              wait_user_timeout_sec_ = 8.0;
    double              max_utterance_sec_ = 15.0;
    int                 tts_cooldown_ms_ = 500;
    bool                enable_barge_in_ = true;
    int                 barge_in_hold_ms_ = 300;
    int                 barge_in_guard_ms_ = 500;
    int                 barge_in_count_ = 0;
    std::atomic<int64_t> barge_in_guard_until_ms_{0};
    bool                wake_prompt_active_ = false;
    int64_t             listening_vad_guard_until_ms_ = 0;

    std::mutex pipeline_player_mutex_;
    pid_t pipeline_player_pid_ = 0;

    static constexpr uint8_t LED_IDLE      = 0;
    static constexpr uint8_t LED_LISTENING = 2;
    static constexpr uint8_t LED_THINKING  = 3;
    static constexpr uint8_t LED_SPEAKING  = 4;

    // ====== TTS：edge-tts → piper → espeak-ng（三级 fallback）======
    int      edge_tts_failures_  = 0;
    int64_t  edge_tts_skip_until_ = 0;
    std::string tts_device_;  // 音频输出设备（所有引擎共用）

    static constexpr int    EDGE_MAX_FAILURES = 2;
    static constexpr int64_t EDGE_RETRY_MS    = 30000;

    bool tts_speak(const std::string& text) {
        // 1. edge-tts（联网高质量）
        if (try_edge_tts() && tts_edge_->speak(text)) {
            edge_tts_failures_ = 0;
            return true;
        }
        edge_tts_failures_++;
        if (edge_tts_failures_ >= EDGE_MAX_FAILURES)
            edge_tts_skip_until_ = now_ms() + EDGE_RETRY_MS;

        // 2. piper（离线 CPU，自然度好）
        if (tts_piper_->speak(text)) return true;

        // 3. espeak-ng（始终可用，机械音但能响）
        return tts_espeak(text);
    }

    bool tts_speak_async(const std::string& text) {
        if (try_edge_tts() && tts_edge_->speak_async(text)) {
            edge_tts_failures_ = 0;
            return true;
        }
        edge_tts_failures_++;
        if (edge_tts_failures_ >= EDGE_MAX_FAILURES)
            edge_tts_skip_until_ = now_ms() + EDGE_RETRY_MS;

        if (tts_piper_->speak_async(text)) return true;
        return tts_espeak(text);  // espeak 阻塞播完
    }

    void tts_wait_done() {
        if (tts_edge_) tts_edge_->wait_done();
        if (tts_piper_) tts_piper_->wait_done();
    }
    void tts_stop() {
        if (tts_edge_) tts_edge_->stop();
        if (tts_piper_) tts_piper_->stop();
    }

    bool tts_is_playing() {
        return (tts_edge_ && tts_edge_->is_playing()) ||
               (tts_piper_ && tts_piper_->is_playing());
    }

    // ---- 流水线辅助：仅合成 / 仅播放（用于断句无缝衔接）----
    // 用 edge-tts → piper 把一句合成到文件，返回文件路径（失败返回空串）
    std::string synth_sentence(const std::string& text, int idx) {
        // edge-tts 优先
        std::string mp3 = "/tmp/va_pipe_" + std::to_string(idx) + ".mp3";
        if (try_edge_tts() && tts_edge_->synthesize(text, mp3)) {
            edge_tts_failures_ = 0;
            return mp3;
        }
        edge_tts_failures_++;
        if (edge_tts_failures_ >= EDGE_MAX_FAILURES)
            edge_tts_skip_until_ = now_ms() + EDGE_RETRY_MS;
        // piper 兜底
        std::string wav = "/tmp/va_pipe_" + std::to_string(idx) + ".wav";
        if (tts_piper_->synthesize(text, wav)) return wav;
        return "";
    }

    bool generation_active(uint64_t generation) const {
        return worker_running_.load() &&
               conversation_generation_.load() == generation;
    }

    // 阻塞播放一个已合成文件；PID 可由 barge-in 路径立即终止。
    bool play_file(const std::string& file, uint64_t generation) {
        if (!generation_active(generation)) return false;
        barge_in_guard_until_ms_.store(now_ms() + barge_in_guard_ms_);
        pid_t pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            if (!tts_device_.empty()) setenv("AUDIODEV", tts_device_.c_str(), 1);
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
                   "-loglevel", "quiet", "-af", "channelmap=0-0|0-1",
                   file.c_str(), nullptr);
            _exit(1);
        }
        {
            std::lock_guard<std::mutex> lock(pipeline_player_mutex_);
            pipeline_player_pid_ = pid;
            if (!generation_active(generation)) kill(pid, SIGTERM);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        {
            std::lock_guard<std::mutex> lock(pipeline_player_mutex_);
            if (pipeline_player_pid_ == pid) pipeline_player_pid_ = 0;
        }
        return generation_active(generation) && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0;
    }

    void stop_all_playback() {
        tts_stop();
        std::lock_guard<std::mutex> lock(pipeline_player_mutex_);
        if (pipeline_player_pid_ > 0) kill(pipeline_player_pid_, SIGTERM);
    }

    // espeak-ng 最终防线：始终可用，机械音但不需要网络/GPU
    bool tts_espeak(const std::string& text, uint64_t generation = 0) {
        std::string wav = "/tmp/va_espeak.wav";
        std::string cmd = "espeak-ng " + escape_shell(text)
                        + " -v zh -w " + wav + " 2>/dev/null";
        if (system(cmd.c_str()) != 0) {
            std::cerr << "[espeak] 合成失败" << std::endl;
            return false;
        }
        RCLCPP_INFO(get_logger(), "espeak-ng 播放 (离线最终防线)");

        if (generation == 0) generation = conversation_generation_.load();
        return play_file(wav, generation);
    }

    // Shell 参数转义（防止文本中的特殊字符破坏命令）
    static std::string escape_shell(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\' || c == '$' || c == '`')
                out += '\\';
            out += c;
        }
        out += '"';
        return out;
    }

    // 是否应该尝试 edge-tts（= 没被冷却 或 到了重试时间）
    bool try_edge_tts() {
        if (edge_tts_failures_ < EDGE_MAX_FAILURES) return true;
        if (now_ms() >= edge_tts_skip_until_) {
            edge_tts_failures_ = 0;  // 冷却结束，重试一次
            return true;
        }
        return false;  // 冷却中，直接跳过
    }
    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ====== 唤醒词 ======
    // 唤醒应答：说"我在，有什么可以帮你"（替代原来的"叮"提示音）
    void speak_wake_response() {
        wake_prompt_active_ = tts_speak_async("我在，有什么可以帮你") &&
                              tts_is_playing();
    }

    // 从识别文本里去掉开头的唤醒词（容忍前导空白），避免把唤醒词传给 LLM
    std::string strip_wake_word(const std::string& text) const {
        if (!use_wake_word_ || wake_word_.empty()) return text;
        size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        if (text.compare(start, wake_word_.size(), wake_word_) == 0)
            return text.substr(start + wake_word_.size());
        return text;
    }

    // 是否为告别语（识别到则回复后退出连续对话）
    static bool is_goodbye(const std::string& text) {
        static const std::vector<std::string> words = {"再见", "拜拜","没事了","晚安", "下次见"};
        for (const auto& w : words)
            if (text.find(w) != std::string::npos)
                return true;
        return false;
    }

    // ====== 回调 ======
    void on_vad(const std_msgs::msg::Bool::SharedPtr msg) {
        last_vad_.store(msg->data);
    }
    void on_audio(const std_msgs::msg::Int16MultiArray::SharedPtr msg) {
        // 唤醒词检测：仅在 IDLE 时喂给专用识别器（只在 executor 线程访问，无竞争）
        State current = state_.load();
        if (current == State::IDLE && wake_asr_ &&
            now_ms() >= cooldown_until_ms_.load())
            wake_asr_->feed(msg->data);

        std::lock_guard<std::mutex> lock(audio_mutex_);
        // 预录缓冲：任何时候都存，保留最近 N ms 的音频
        preroll_buffer_.insert(preroll_buffer_.end(), msg->data.begin(), msg->data.end());
        size_t max_preroll = static_cast<size_t>(preroll_ms_ * 16);  // 16000 Hz * ms/1000
        while (preroll_buffer_.size() > max_preroll)
            preroll_buffer_.pop_front();

        if (current == State::LISTENING)
            audio_buffer_.insert(audio_buffer_.end(), msg->data.begin(), msg->data.end());
    }

    // ====== 定时器 ======
    void on_tick() {
        switch (state_.load()) {
            case State::IDLE:      tick_idle();      break;
            case State::LISTENING: tick_listening();  break;
            case State::SPEAKING:  tick_speaking();   break;
            case State::THINKING:                     break;
        }
    }

    void tick_idle() {
        if (now_ms() < cooldown_until_ms_.load()) return;

        // 连续对话超时：唤醒后超过超时时间无成功识别 → 重新锁定
        if (in_conversation_.load()) {
            int64_t last = last_recognized_ms_.load();
            if (last > 0 && now_ms() - last > (int64_t)(conversation_timeout_sec_ * 1000)) {
                RCLCPP_INFO(get_logger(), "连续对话超时，回到待唤醒");
                in_conversation_.store(false);
                if (wake_asr_) wake_asr_->reset();
            }
        }

        // 待唤醒态：只认唤醒词
        if (!in_conversation_.load() && use_wake_word_ && wake_asr_) {
            std::string p = wake_asr_->partial_result();
            if (!p.empty() && p.find(wake_word_) != std::string::npos) {
                RCLCPP_INFO(get_logger(), "唤醒词触发: %s", p.c_str());
                in_conversation_.store(true);
                last_recognized_ms_.store(now_ms());  // 从唤醒时刻开始计超时
                start_listening();
                speak_wake_response();
            }
            return;
        }

        // 连续对话态（或未启用唤醒词）：语音即可触发
        if (last_vad_.load()) {
            RCLCPP_INFO(get_logger(), "检测到语音，开始录音...");
            start_listening(true);  // 用户已在说话，静音计时从这句说完才开始
        }
    }

    void start_listening(bool already_speaking = false,
                         bool from_barge_in = false) {
        conversation_generation_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            audio_buffer_.clear();
            // 把预录缓冲（唤醒词/触发前的音频）补到开头
            size_t keep = preroll_buffer_.size();
            if (from_barge_in) {
                // VAD 需要持续一段时间才确认插话，保留这段确认窗口，避免截掉句首。
                const size_t barge_preroll = static_cast<size_t>(
                    (barge_in_hold_ms_ + 100) * 16);
                keep = std::min(keep, barge_preroll);
            }
            audio_buffer_.insert(audio_buffer_.end(), preroll_buffer_.end() - keep,
                                 preroll_buffer_.end());
            silence_count_ = 0;
            heard_speech_ = already_speaking;  // 连续对话(VAD触发)时用户已在说话
            listening_started_ms_ = now_ms();
            speech_started_ms_ = already_speaking ? listening_started_ms_ : 0;
            listening_vad_guard_until_ms_ = from_barge_in
                ? listening_started_ms_
                : listening_started_ms_ + tts_cooldown_ms_;
            state_.store(State::LISTENING);
        }
        if (wake_asr_) wake_asr_->reset();  // 停止唤醒检测，等回到 IDLE 再开
    }

    void finish_listening(const char* reason) {
        std::vector<int16_t> audio;
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            audio.assign(audio_buffer_.begin(), audio_buffer_.end());
            audio_buffer_.clear();
        }
        if (audio.empty()) {
            state_.store(State::IDLE);
            return;
        }

        const uint64_t generation = conversation_generation_.load();
        state_.store(State::THINKING);
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            work_queue_.clear();
            work_queue_.push_back({generation, std::move(audio)});
        }
        work_cv_.notify_one();
        RCLCPP_INFO(get_logger(), "%s", reason);
    }

    void tick_listening() {
        if (wake_prompt_active_) {
            if (tts_is_playing()) {
                std::lock_guard<std::mutex> lock(audio_mutex_);
                audio_buffer_.clear();
                return;
            }
            wake_prompt_active_ = false;
            listening_started_ms_ = now_ms();
            speech_started_ms_ = 0;
            listening_vad_guard_until_ms_ = listening_started_ms_ + tts_cooldown_ms_;
        }
        if (now_ms() < listening_vad_guard_until_ms_) {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            audio_buffer_.clear();
            return;
        }

        // 只统计"说完之后的静音"，忽略"还没开始说话"的静音
        // （否则唤醒应答播放期间会被误判为说话结束，用户指令来不及说）
        if (last_vad_.load()) {
            if (!heard_speech_) speech_started_ms_ = now_ms();
            heard_speech_ = true;
            silence_count_ = 0;
        } else if (heard_speech_) {
            silence_count_++;
        }

        const int64_t elapsed = now_ms() - listening_started_ms_;
        if (!heard_speech_ && elapsed >= static_cast<int64_t>(wait_user_timeout_sec_ * 1000)) {
            RCLCPP_WARN(get_logger(), "等待用户说话超时，回到待唤醒");
            in_conversation_.store(false);
            conversation_generation_.fetch_add(1);
            state_.store(State::IDLE);
            cooldown_until_ms_.store(now_ms() + tts_cooldown_ms_);
            if (wake_asr_) wake_asr_->reset();
            return;
        }
        if (heard_speech_ && speech_started_ms_ > 0 &&
            now_ms() - speech_started_ms_ >=
                static_cast<int64_t>(max_utterance_sec_ * 1000)) {
            finish_listening("达到最大录音时长，强制结束本句");
            return;
        }
        if (silence_count_ >= silence_threshold_) {
            finish_listening("说话结束");
        }
    }

    void tick_speaking() {
        if (!enable_barge_in_ || now_ms() < barge_in_guard_until_ms_.load()) {
            barge_in_count_ = 0;
            return;
        }
        if (last_vad_.load()) barge_in_count_++;
        else barge_in_count_ = 0;

        const int required = std::max(1, (barge_in_hold_ms_ + 99) / 100);
        if (barge_in_count_ < required) return;

        RCLCPP_INFO(get_logger(), "检测到用户插话，停止当前回复");
        barge_in_count_ = 0;
        llm_.cancel_current();
        stop_all_playback();
        start_listening(true, true);
    }

    void worker_loop() {
        while (worker_running_.load()) {
            std::pair<uint64_t, std::vector<int16_t>> work;
            {
                std::unique_lock<std::mutex> lock(work_mutex_);
                work_cv_.wait(lock, [this]() {
                    return !worker_running_.load() || !work_queue_.empty();
                });
                if (!worker_running_.load()) break;
                work = std::move(work_queue_.front());
                work_queue_.pop_front();
            }
            process_audio(work.second, work.first);
        }
    }

    // ====== 核心：ASR → LLM → TTS ======
    void process_audio(const std::vector<int16_t>& audio, uint64_t generation) {
        if (!generation_active(generation)) return;
        // 1. ASR
        RCLCPP_INFO(get_logger(), "ASR... (%zu samples)", audio.size());
        static const int CHUNK = 512;
        for (size_t i = 0; i < audio.size(); i += CHUNK) {
            if (!generation_active(generation)) {
                asr_->reset();
                return;
            }
            size_t n = std::min(audio.size() - i, (size_t)CHUNK);
            std::vector<int16_t> chunk(audio.begin() + i, audio.begin() + i + n);
            asr_->feed(chunk);
        }
        std::string text = asr_->final_result();
        asr_->reset();
        if (!generation_active(generation)) return;

        text = strip_wake_word(text);
        if (text.empty()) {
            RCLCPP_WARN(get_logger(), "只听到唤醒词，未识别到指令");
            state_.store(State::IDLE);
            cooldown_until_ms_.store(now_ms() + tts_cooldown_ms_);
            return;
        }
        RCLCPP_INFO(get_logger(), "识别: %s", text.c_str());

        // 识别到告别语 → 简短回复并退出连续对话，回到待唤醒
        if (is_goodbye(text)) {
            RCLCPP_INFO(get_logger(), "识别到告别语，退出连续对话");
            in_conversation_.store(false);
            if (wake_asr_) wake_asr_->reset();
            state_.store(State::SPEAKING);
            barge_in_guard_until_ms_.store(now_ms() + barge_in_guard_ms_);
            std::string goodbye_file = synth_sentence("好的，再见", 0);
            if (!goodbye_file.empty()) {
                play_file(goodbye_file, generation);
                std::remove(goodbye_file.c_str());
            } else {
                tts_espeak("好的，再见", generation);
            }
            if (!generation_active(generation)) return;
            cooldown_until_ms_.store(now_ms() + tts_cooldown_ms_);
            state_.store(State::IDLE);
            RCLCPP_INFO(get_logger(), "已回到待唤醒，等待唤醒词...");
            return;
        }

        // 2. LLM
        std::vector<ChatMessage> request_history;
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            request_history = history_;
            request_history.push_back({"user", text});
        }

        std::string used_name;
        std::string full_reply;
        std::string pending;   // 积累字符，到句子边界就 TTS

        state_.store(State::SPEAKING);
        barge_in_guard_until_ms_.store(now_ms() + barge_in_guard_ms_);
        llm_.reset_cancel();

        // ---- TTS 流水线：合成与播放并行，消除断句之间的空档 ----
        struct SpeakItem { std::string text; std::string file; };
        std::deque<SpeakItem> play_queue;
        std::mutex pq_mutex;
        std::condition_variable pq_cv;
        bool stream_end = false;
        int  sentence_idx = 0;

        // 播放线程：按顺序无缝播放已合成好的句子
        std::thread speaker([&]() {
            while (true) {
                SpeakItem item;
                {
                    std::unique_lock<std::mutex> lk(pq_mutex);
                    pq_cv.wait_for(lk, std::chrono::milliseconds(100), [&]() {
                        return !play_queue.empty() || stream_end ||
                               !generation_active(generation);
                    });
                    if (!generation_active(generation)) {
                        for (const auto& queued : play_queue)
                            if (!queued.file.empty()) std::remove(queued.file.c_str());
                        play_queue.clear();
                        break;
                    }
                    if (play_queue.empty() && stream_end) break;
                    if (play_queue.empty()) continue;
                    item = std::move(play_queue.front());
                    play_queue.pop_front();
                }
                if (!item.file.empty()) {
                    play_file(item.file, generation);  // 阻塞播完或被插话终止
                    std::remove(item.file.c_str());  // 播完清理临时文件
                } else {
                    tts_espeak(item.text, generation);  // 合成失败的最后防线
                }
            }
        });

        bool ok = llm_.chat_stream(request_history,
            [&](const std::string& token) {
                if (!generation_active(generation)) return;
                full_reply += token;
                pending += token;

                // 检测句子边界（中英文标点 + 换行）
                // 注意：不能按 char 遍历判断中文标点——UTF-8 下中文标点是
                // 多字节，'。' 这种写法是 multi-character constant，永远匹配不上。
                bool boundary =
                    token.find('!')  != std::string::npos ||
                    token.find('?')  != std::string::npos ||
                    token.find('\n') != std::string::npos ||
                    token.find("。") != std::string::npos ||
                    token.find("！") != std::string::npos ||
                    token.find("？") != std::string::npos;
                // 积累超过 60 字符也切开
                if (pending.size() >= 60) boundary = true;

                if (boundary && !pending.empty() && generation_active(generation)) {
                    std::string sentence = std::move(pending);
                    pending.clear();
                    // 合成下一句（与上一句的播放并行进行，不阻塞播放）
                    std::string file = synth_sentence(sentence, sentence_idx++);
                    if (!generation_active(generation)) {
                        if (!file.empty()) std::remove(file.c_str());
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk(pq_mutex);
                        play_queue.push_back({std::move(sentence), file});
                    }
                    pq_cv.notify_one();
                }
            },
            used_name);

        // 播剩余文本
        if (!pending.empty() && generation_active(generation)) {
            std::string file = synth_sentence(pending, sentence_idx++);
            {
                std::lock_guard<std::mutex> lk(pq_mutex);
                play_queue.push_back({std::move(pending), file});
            }
            pq_cv.notify_one();
        }

        // 通知播放线程流结束，并等全部播完再切状态
        {
            std::lock_guard<std::mutex> lk(pq_mutex);
            stream_end = true;
        }
        pq_cv.notify_all();
        speaker.join();

        if (!generation_active(generation)) return;

        if (!full_reply.empty()) {
            RCLCPP_INFO(get_logger(), "回复(%s): %s",
                        used_name.c_str(), full_reply.c_str());
        } else if (!ok) {
            full_reply = "抱歉，无法回答";
            std::string fallback_file = synth_sentence(full_reply, 0);
            if (!fallback_file.empty()) {
                play_file(fallback_file, generation);
                std::remove(fallback_file.c_str());
            } else {
                tts_espeak(full_reply, generation);
            }
            if (!generation_active(generation)) return;
        }

        // 存历史
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_.push_back({"user", text});
            history_.push_back({"assistant", full_reply});
            while ((int)history_.size() > 1 + max_history_ * 2)
                history_.erase(history_.begin() + 1);
        }

        // 回答已全部播完，从这里开始计连续对话超时
        last_recognized_ms_.store(now_ms());
        cooldown_until_ms_.store(now_ms() + tts_cooldown_ms_);
        state_.store(State::IDLE);
        RCLCPP_INFO(get_logger(), "就绪");
    }
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<InteractionNode>();
    if (!node->is_ready()) {
        RCLCPP_ERROR(rclcpp::get_logger("voice_interaction_node"),
                     "初始化失败（模型加载），退出");
        rclcpp::shutdown();
        return 1;
    }
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
