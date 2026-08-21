#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int16_multi_array.hpp>

#include "llm_client.hpp"
#include "session_state_machine.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

class InteractionNode : public rclcpp::Node {
public:
    InteractionNode() : Node("voice_interaction_node") {
        const char* home_env = std::getenv("HOME");
        const std::string home = home_env ? home_env : "/home/lsj";

        SessionStateMachineConfig config;
        config.recorder.asr_model_dir = declare_parameter(
            "asr_model_dir",
            home + "/ReSpearMicArray/models/"
                  "sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20");
        config.system_prompt =
            declare_parameter("system_prompt", "你好你是一个语音助手。");

        const double silence_sec = declare_parameter("silence_sec", 1.0);
        config.recorder.preroll_ms = declare_parameter("preroll_ms", 500);
        config.recorder.wake_preroll_ms =
            declare_parameter("wake_preroll_ms", 2000);
        config.max_history = declare_parameter("max_history", 6);
        config.recorder.wait_user_timeout_sec =
            declare_parameter("wait_user_timeout_sec", 8.0);
        config.recorder.max_utterance_sec =
            declare_parameter("max_utterance_sec", 15.0);
        config.tts_cooldown_ms = declare_parameter("tts_cooldown_ms", 500);
        config.enable_barge_in = declare_parameter("enable_barge_in", true);
        config.barge_in_hold_ms = declare_parameter("barge_in_hold_ms", 300);
        config.barge_in_guard_ms = declare_parameter("barge_in_guard_ms", 500);

        config.recorder.preroll_ms = std::max(0, config.recorder.preroll_ms);
        config.max_history = std::max(0, config.max_history);
        config.recorder.silence_threshold =
            std::max(1, static_cast<int>(silence_sec * 10));

        config.recorder.use_wake_word =
            declare_parameter("use_wake_word", true);
        config.recorder.wake_detector =
            declare_parameter("wake_detector", "kws");
        config.recorder.wake_word = declare_parameter("wake_word", "你好");
        config.recorder.wake_word_aliases =
            declare_parameter<std::vector<std::string>>(
                "wake_word_aliases", std::vector<std::string>{});
        config.recorder.kws_model_dir = declare_parameter(
            "kws_model_dir",
            home + "/ReSpearMicArray/models/"
                  "sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01-mobile");
        config.recorder.kws_encoder = declare_parameter("kws_encoder", "");
        config.recorder.kws_decoder = declare_parameter("kws_decoder", "");
        config.recorder.kws_joiner = declare_parameter("kws_joiner", "");
        config.recorder.kws_tokens = declare_parameter("kws_tokens", "");
        config.recorder.kws_keywords_file = declare_parameter(
            "kws_keywords_file",
            home + "/ReSpearMicArray/voice_interaction/config/wake_words.txt");
        config.recorder.kws_num_threads =
            declare_parameter("kws_num_threads", 1);
        config.recorder.kws_max_active_paths =
            declare_parameter("kws_max_active_paths", 4);
        config.recorder.kws_num_trailing_blanks =
            declare_parameter("kws_num_trailing_blanks", 1);
        config.recorder.kws_keywords_score =
            declare_parameter("kws_keywords_score", 3.0);
        config.recorder.kws_keywords_threshold =
            declare_parameter("kws_keywords_threshold", 0.1);
        config.conversation_timeout_sec =
            declare_parameter("conversation_timeout_sec", 20.0);

        const std::string http_proxy =
            declare_parameter("http_proxy", "");
        auto llm = std::make_unique<LLMClient>();
        if (!http_proxy.empty()) {
            llm->set_proxy(http_proxy);
            RCLCPP_INFO(get_logger(), "已设置 HTTP 代理: %s",
                        http_proxy.c_str());
        }

        const char* gateway_key_env =
            std::getenv("VOICE_GATEWAY_API_KEY");
        const std::string gateway_key =
            gateway_key_env ? gateway_key_env : "";
        const std::string gateway_url =
            declare_parameter("gateway_base_url", "");
        const std::string gateway_model =
            declare_parameter("gateway_model", "");
        if (!gateway_key.empty() && !gateway_url.empty() &&
            !gateway_model.empty()) {
            llm->add_provider(
                {"网关(OpenAI)", "openai", gateway_url, gateway_key,
                 gateway_model});
            RCLCPP_INFO(get_logger(), "已注册网关");
        } else if (!gateway_url.empty() || !gateway_model.empty() ||
                   !gateway_key.empty()) {
            RCLCPP_WARN(
                get_logger(),
                "网关配置不完整，需同时设置 gateway_base_url、gateway_model "
                "和 VOICE_GATEWAY_API_KEY");
        }

        const bool use_ollama = declare_parameter("use_ollama", true);
        if (use_ollama) {
            const std::string ollama_url = declare_parameter(
                "ollama_url", "http://localhost:11434/v1");
            const std::string ollama_model = declare_parameter(
                "ollama_model", "qwen2.5:1.5b");
            llm->add_provider(
                {"Ollama", "openai", ollama_url, "ollama", ollama_model});
            RCLCPP_INFO(get_logger(), "已注册 Ollama 本地: %s @ %s",
                        ollama_model.c_str(), ollama_url.c_str());
        } else {
            RCLCPP_INFO(get_logger(), "Ollama 已禁用（use_ollama=false）");
        }

        config.playback.tts_device =
            declare_parameter("tts_device", "");
        config.playback.tts_voice =
            declare_parameter("tts_voice", "zh-CN-XiaoxiaoNeural");
        config.playback.piper_model = declare_parameter(
            "piper_model", home + "/ReSpearMicArray/zh_CN-huayan-medium.onnx");
        if (config.playback.tts_device.empty() ||
            config.playback.tts_device == "default") {
            RCLCPP_INFO(get_logger(), "TTS 输出设备: system default");
        } else {
            RCLCPP_INFO(get_logger(), "TTS 输出设备: %s",
                        config.playback.tts_device.c_str());
        }

        session_ = std::make_unique<SessionStateMachine>(
            get_logger(), config, std::move(llm));
        if (!session_->is_ready()) {
            RCLCPP_ERROR(get_logger(), "语音交互会话初始化失败");
            return;
        }

        sub_vad_ = create_subscription<std_msgs::msg::Bool>(
            "vad", 10,
            [this](const std_msgs::msg::Bool::SharedPtr msg) {
                session_->on_vad(msg->data);
            });
        sub_audio_ = create_subscription<std_msgs::msg::Int16MultiArray>(
            "audio_raw", 10,
            [this](const std_msgs::msg::Int16MultiArray::SharedPtr msg) {
                session_->on_audio(msg->data);
            });
        timer_ = create_wall_timer(
            std::chrono::milliseconds(100),
            [this]() { session_->on_tick(); });

        RCLCPP_INFO(get_logger(), "语音交互节点启动完成，等待说话...");
    }

    ~InteractionNode() override {
        if (session_) session_->shutdown();
    }

    bool is_ready() const {
        return session_ && session_->is_ready();
    }

private:
    std::unique_ptr<SessionStateMachine> session_;
    rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr sub_vad_;
    rclcpp::Subscription<std_msgs::msg::Int16MultiArray>::SharedPtr sub_audio_;
    rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<InteractionNode>();
    if (!node->is_ready()) {
        RCLCPP_ERROR(rclcpp::get_logger("voice_interaction_node"),
                     "初始化失败，退出");
        rclcpp::shutdown();
        return 1;
    }
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
