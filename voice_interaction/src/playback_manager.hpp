#pragma once

#include <rclcpp/rclcpp.hpp>

#include "tts_edge.hpp"
#include "tts_piper.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>

class PlaybackManager {
public:
    struct Config {
        std::string tts_voice = "zh-CN-XiaoxiaoNeural";
        std::string tts_device;
        std::string piper_model;
    };

    PlaybackManager(const rclcpp::Logger& logger, const Config& config);
    ~PlaybackManager();

    bool synthesize(const std::string& text, std::string& output_path);
    bool speak(const std::string& text,
               const std::function<bool()>& should_continue = {});
    bool speak_async(const std::string& text);
    bool wait_until_done(const std::function<bool()>& should_continue = {});

    bool play_file(const std::string& file,
                   const std::function<bool()>& should_continue = {});
    void stop();
    bool is_playing() const;

private:
    rclcpp::Logger logger_;
    Config config_;
    std::unique_ptr<TTSEdge> edge_;
    std::unique_ptr<TTSPiper> piper_;

    mutable std::mutex synthesis_mutex_;
    mutable std::mutex pipeline_mutex_;
    pid_t pipeline_player_pid_ = 0;
    std::atomic<uint64_t> file_counter_{0};

    int edge_tts_failures_ = 0;
    int64_t edge_tts_skip_until_ms_ = 0;
    static constexpr int kEdgeMaxFailures = 2;
    static constexpr int64_t kEdgeRetryMs = 30000;

    static std::string sanitize_tts_text(const std::string& input);
    static std::string escape_shell(const std::string& input);
    static int64_t now_ms();
    bool try_edge_tts();
    bool espeak_speak(const std::string& text,
                      const std::function<bool()>& should_continue);
    std::string unique_path(const std::string& extension);
};
