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

        silence_threshold_ = static_cast<int>(silence_sec * 10);
        state_ = State::IDLE;

        RCLCPP_INFO(get_logger(), "语音交互节点启动完成，等待说话...");
    }

    /** 初始化是否成功（模型加载失败时返回 false） */
    bool is_ready() const { return asr_ && asr_->is_ready(); }

private:
    State state_;
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
    int                 cooldown_ = 0;
    int                 preroll_ms_ = 500;
    bool                last_vad_ = false;

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
        tts_edge_->wait_done();
        tts_piper_->wait_done();
    }
    void tts_stop() {
        tts_edge_->stop();
        tts_piper_->stop();
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

    // 阻塞播放一个已合成文件（ffplay 按扩展名自动识别 wav/mp3）
    void play_file(const std::string& file) {
        std::string play;
        if (!tts_device_.empty())
            play = "AUDIODEV=" + tts_device_
                 + " ffplay -nodisp -autoexit -loglevel quiet"
                 + " -af \"channelmap=0-0|0-1\" " + file;
        else
            play = std::string("ffplay -nodisp -autoexit -loglevel quiet")
                 + " -af \"channelmap=0-0|0-1\" " + file;
        system(play.c_str());
    }

    // espeak-ng 最终防线：始终可用，机械音但不需要网络/GPU
    bool tts_espeak(const std::string& text) {
        std::string wav = "/tmp/va_espeak.wav";
        std::string cmd = "espeak-ng " + escape_shell(text)
                        + " -v zh -w " + wav + " 2>/dev/null";
        if (system(cmd.c_str()) != 0) {
            std::cerr << "[espeak] 合成失败" << std::endl;
            return false;
        }
        RCLCPP_INFO(get_logger(), "espeak-ng 播放 (离线最终防线)");

        std::string play;
        if (!tts_device_.empty())
            play = "AUDIODEV=" + tts_device_
                 + " ffplay -nodisp -autoexit -loglevel quiet"
                 + " -af \"channelmap=0-0|0-1\" " + wav;
        else
            play = std::string("ffplay -nodisp -autoexit -loglevel quiet")
                 + " -af \"channelmap=0-0|0-1\" " + wav;
        system(play.c_str());
        return true;
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
        tts_speak_async("我在，有什么可以帮你");
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
    void on_vad(const std_msgs::msg::Bool::SharedPtr msg)   { last_vad_ = msg->data; }
    void on_audio(const std_msgs::msg::Int16MultiArray::SharedPtr msg) {
        // 唤醒词检测：仅在 IDLE 时喂给专用识别器（只在 executor 线程访问，无竞争）
        if (state_ == State::IDLE && wake_asr_)
            wake_asr_->feed(msg->data);

        std::lock_guard<std::mutex> lock(audio_mutex_);
        // 预录缓冲：任何时候都存，保留最近 N ms 的音频
        preroll_buffer_.insert(preroll_buffer_.end(), msg->data.begin(), msg->data.end());
        size_t max_preroll = static_cast<size_t>(preroll_ms_ * 16);  // 16000 Hz * ms/1000
        while (preroll_buffer_.size() > max_preroll)
            preroll_buffer_.pop_front();

        if (state_ == State::LISTENING)
            audio_buffer_.insert(audio_buffer_.end(), msg->data.begin(), msg->data.end());
    }

    // ====== 定时器 ======
    void on_tick() {
        switch (state_) {
            case State::IDLE:      tick_idle();      break;
            case State::LISTENING: tick_listening();  break;
            case State::SPEAKING:                     break;
            case State::THINKING:                     break;
        }
    }

    void tick_idle() {
        if (cooldown_ > 0) { cooldown_--; return; }

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
        if (last_vad_) {
            RCLCPP_INFO(get_logger(), "检测到语音，开始录音...");
            start_listening(true);  // 用户已在说话，静音计时从这句说完才开始
        }
    }

    void start_listening(bool already_speaking = false) {
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            audio_buffer_.clear();
            // 把预录缓冲（唤醒词/触发前的音频）补到开头
            audio_buffer_.insert(audio_buffer_.end(),
                                 preroll_buffer_.begin(), preroll_buffer_.end());
            silence_count_ = 0;
            heard_speech_ = already_speaking;  // 连续对话(VAD触发)时用户已在说话
            state_ = State::LISTENING;
        }
        if (wake_asr_) wake_asr_->reset();  // 停止唤醒检测，等回到 IDLE 再开
    }

    void tick_listening() {
        // 只统计"说完之后的静音"，忽略"还没开始说话"的静音
        // （否则唤醒应答播放期间会被误判为说话结束，用户指令来不及说）
        if (last_vad_) {
            heard_speech_ = true;
            silence_count_ = 0;
        } else if (heard_speech_) {
            silence_count_++;
        }

        if (silence_count_ >= silence_threshold_) {
            RCLCPP_INFO(get_logger(), "说话结束");
            std::vector<int16_t> audio;
            {
                std::lock_guard<std::mutex> lock(audio_mutex_);
                audio.assign(audio_buffer_.begin(), audio_buffer_.end());
                audio_buffer_.clear();
            }
            if (audio.empty()) { state_ = State::IDLE; return; }

            state_ = State::THINKING;
            std::thread([this, a = std::move(audio)]() { process_audio(a); }).detach();
        }
    }

    // ====== 核心：ASR → LLM → TTS ======
    void process_audio(const std::vector<int16_t>& audio) {
        // 1. ASR
        RCLCPP_INFO(get_logger(), "ASR... (%zu samples)", audio.size());
        static const int CHUNK = 512;
        for (size_t i = 0; i < audio.size(); i += CHUNK) {
            size_t n = std::min(audio.size() - i, (size_t)CHUNK);
            std::vector<int16_t> chunk(audio.begin() + i, audio.begin() + i + n);
            asr_->feed(chunk);
        }
        std::string text = asr_->final_result();
        asr_->reset();

        text = strip_wake_word(text);
        if (text.empty()) {
            RCLCPP_WARN(get_logger(), "只听到唤醒词，未识别到指令");
            state_ = State::IDLE;
            return;
        }
        RCLCPP_INFO(get_logger(), "识别: %s", text.c_str());

        // 识别到告别语 → 简短回复并退出连续对话，回到待唤醒
        if (is_goodbye(text)) {
            RCLCPP_INFO(get_logger(), "识别到告别语，退出连续对话");
            in_conversation_.store(false);
            if (wake_asr_) wake_asr_->reset();
            state_ = State::SPEAKING;
            tts_speak("好的，再见");
            state_ = State::IDLE;
            RCLCPP_INFO(get_logger(), "已回到待唤醒，等待唤醒词...");
            return;
        }

        // 2. LLM
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_.push_back({"user", text});
            while ((int)history_.size() > 1 + max_history_ * 2)
                history_.erase(history_.begin() + 1);
        }

        std::string used_name;
        std::string full_reply;
        std::string pending;   // 积累字符，到句子边界就 TTS

        state_ = State::SPEAKING;

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
                    pq_cv.wait(lk, [&]() { return !play_queue.empty() || stream_end; });
                    if (play_queue.empty() && stream_end) break;
                    item = std::move(play_queue.front());
                    play_queue.pop_front();
                }
                if (!item.file.empty()) {
                    play_file(item.file);            // 阻塞播完
                    std::remove(item.file.c_str());  // 播完清理临时文件
                } else {
                    tts_espeak(item.text);           // 合成失败的最后防线（阻塞）
                }
            }
        });

        bool ok = llm_.chat_stream(history_,
            [&](const std::string& token) {
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

                if (boundary && !pending.empty()) {
                    std::string sentence = std::move(pending);
                    pending.clear();
                    // 合成下一句（与上一句的播放并行进行，不阻塞播放）
                    std::string file = synth_sentence(sentence, sentence_idx++);
                    {
                        std::lock_guard<std::mutex> lk(pq_mutex);
                        play_queue.push_back({std::move(sentence), file});
                    }
                    pq_cv.notify_one();
                }
            },
            used_name);

        // 播剩余文本
        if (!pending.empty()) {
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

        if (!full_reply.empty()) {
            RCLCPP_INFO(get_logger(), "回复(%s): %s",
                        used_name.c_str(), full_reply.c_str());
        } else {
            full_reply = "抱歉，无法回答";
            tts_speak(full_reply);
        }

        // 存历史
        {
            std::lock_guard<std::mutex> lock(history_mutex_);
            history_.push_back({"assistant", full_reply});
        }

        // 回答已全部播完，从这里开始计连续对话超时
        last_recognized_ms_.store(now_ms());

        state_ = State::IDLE;
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
