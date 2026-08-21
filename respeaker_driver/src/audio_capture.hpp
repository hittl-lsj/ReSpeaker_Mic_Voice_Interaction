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
    /**
     * @param device_name ALSA 设备名；为空或 "auto" 时自动查找 ReSpeaker
     * @param input_ch 采集声道数，当前节点使用单声道
     * @param sample_rate 采样率，单位 Hz
     * @param frames_per_buffer 每次读取的 PCM frame 数
     */
    AudioCapture(const std::string& device_name = "",
                 int input_ch = 1, int sample_rate = 16000,
                 int frames_per_buffer = 512);
    ~AudioCapture();

    /** 设置 ALSA 设备名；必须在 start() 之前调用 */
    void set_device_name(const std::string& device_name);

    /** 打开并配置 ALSA 采集流 */
    bool start();

    /** 停止采集并释放 ALSA 句柄 */
    void stop();

    /** 返回采集流是否处于运行状态 */
    bool is_running() const;

    /**
     * @brief 读取一批 PCM 音频
     * @param buffer 输出缓冲区，单位为 int16 PCM sample
     * @return 实际读取的 frame 数；暂时无数据返回 0，失败返回 -1
     */
    int read(std::vector<int16_t>& buffer);

private:
    std::string device_name_;
    int  input_ch_;
    int  sample_rate_;
    int  frames_per_buffer_;
    void* stream_;   // snd_pcm_t* (ALSA PCM handle)
    bool running_;
};
