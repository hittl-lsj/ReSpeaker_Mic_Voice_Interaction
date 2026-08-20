#pragma once

#include <cstdint>
#include <vector>
#include <string>

/**
 * @brief 音频采集驱动（基于 ALSA 直驱）
 *
 * 从 ReSpeaker 采集处理后音频（Channel 0）。
 * 不依赖 ROS2，可独立测试。
 */
class AudioCapture {
public:
    AudioCapture(const std::string& device_name = "ReSpeaker",
                 int input_ch = 1, int sample_rate = 16000,
                 int frames_per_buffer = 512);
    ~AudioCapture();
    bool start();
    void stop();
    bool is_running() const;
    int read(std::vector<int16_t>& buffer);

private:
    std::string device_name_;
    int  input_ch_;
    int  sample_rate_;
    int  frames_per_buffer_;
    void* stream_;   // snd_pcm_t* (ALSA PCM handle)
    bool running_;
};
