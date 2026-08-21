#pragma once

#include <cstdint>
#include <vector>
#include <string>

/**
 * @brief 音频采集驱动（基于 ALSA 直驱）
 *
 * 从 ALSA 采集 16-bit mono PCM 音频。
 * 不依赖 ROS2，可独立测试。
 */
class AudioCapture {
public:
    AudioCapture(const std::string& device_name = "",
                 int input_ch = 1, int sample_rate = 16000,
                 int frames_per_buffer = 512);
    ~AudioCapture();
    void set_device_name(const std::string& device_name);
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
