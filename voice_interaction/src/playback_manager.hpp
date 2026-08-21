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
        std::string tts_voice = "zh-CN-XiaoxiaoNeural";  // edge-tts 发音人
        std::string tts_device;  // 空或 default 使用系统默认输出
        std::string piper_model; // Piper 的 .onnx 模型路径
    };

    /** 创建 edge-tts、Piper 和音频播放所需的管理器。 */
    PlaybackManager(const rclcpp::Logger& logger, const Config& config);
    ~PlaybackManager();

    /**
     * @brief 合成到文件但不播放
     * @param text 待合成文本
     * @param output_path 输出文件路径，由调用方负责删除
     */
    bool synthesize(const std::string& text, std::string& output_path);

    /**
     * @brief 合成并阻塞播放
     * @param text 待播报文本
     * @param should_continue 返回 false 时取消当前播放
     */
    bool speak(const std::string& text,
               const std::function<bool()>& should_continue = {});

    /** 合成并启动后台播放，不等待播放结束。 */
    bool speak_async(const std::string& text);

    /**
     * @brief 等待当前音频播放完成
     * @param should_continue 返回 false 时停止播放并返回 false
     */
    bool wait_until_done(const std::function<bool()>& should_continue = {});

    /**
     * @brief 播放已有音频文件
     * @param file 音频文件路径
     * @param should_continue 播放期间的取消检查回调
     */
    bool play_file(const std::string& file,
                   const std::function<bool()>& should_continue = {});

    /** 停止 edge-tts、Piper 或流水线播放器产生的音频进程。 */
    void stop();

    /** 当前是否存在正在播放的音频进程。 */
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
