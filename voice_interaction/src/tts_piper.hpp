#pragma once
#include <string>
#include <atomic>

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
    TTSPiper(const std::string& model_path = "zh_CN-huayan-medium.onnx",
             const std::string& audio_dev = "");
    ~TTSPiper();

    bool speak(const std::string& text);
    bool speak_async(const std::string& text);
    // 仅合成到指定 wav（不播放），用于流水线（合成与播放并行）
    bool synthesize(const std::string& text, const std::string& wav_path);
    void wait_done();
    void stop();

private:
    std::string model_path_;
    std::string audio_dev_;
    std::string tmp_txt_;
    std::string tmp_wav_;
    int         ffplay_pid_ = 0;
    std::atomic<bool> playing_{false};

    void kill_ffplay();
};
