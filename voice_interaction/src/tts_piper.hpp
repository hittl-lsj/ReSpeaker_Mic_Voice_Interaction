#pragma once
#include <string>
#include <atomic>
#include <mutex>

/**
 * @brief 离线 TTS：piper（纯 CPU、毫秒级合成）
 *
 * 模型下载：
 *   pip3 install piper-tts --break-system-packages
 *   python3 -m piper.download_voices zh_CN-huayan-medium
 *
 * 特点：不需要 GPU、不需要网络、延迟 <100ms
 */
class TTSPiper {
public:
    /**
     * @param model_path Piper ONNX 模型路径
     * @param audio_dev ALSA 输出设备；为空或 "default" 使用系统默认输出
     */
    TTSPiper(const std::string& model_path = "zh_CN-huayan-medium.onnx",
             const std::string& audio_dev = "");
    ~TTSPiper();

    /** 合成并阻塞播放一段文本。 */
    bool speak(const std::string& text);

    /** 合成并启动后台播放，不等待播放结束。 */
    bool speak_async(const std::string& text);

    /**
     * @brief 仅合成到指定 wav，不播放
     * @param text 待合成文本
     * @param wav_path 输出 wav 路径
     */
    bool synthesize(const std::string& text, const std::string& wav_path);

    /** 等待当前 ffplay 播放进程结束。 */
    void wait_done();

    /** 终止当前 ffplay 播放进程。 */
    void stop();

    /** 当前是否正在播放。 */
    bool is_playing();

private:
    std::string model_path_;
    std::string audio_dev_;
    std::string tmp_txt_;
    std::string tmp_wav_;
    std::atomic<int> ffplay_pid_{0};
    std::atomic<bool> playing_{false};
    mutable std::mutex process_mutex_;

    void kill_ffplay();
};
