#include "audio_capture.hpp"
#include <iostream>
#include <cstring>
#include <alsa/asoundlib.h>

AudioCapture::AudioCapture(const std::string& device_name,
                           int input_ch, int sample_rate, int frames_per_buffer)
    : device_name_(device_name)
    , input_ch_(input_ch)
    , sample_rate_(sample_rate)
    , frames_per_buffer_(frames_per_buffer)
    , stream_(nullptr)
    , running_(false)
{
    // ALSA 不需要全局初始化,每个 PCM 句柄独立打开
}

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::start() {
    if (running_) return true;

    std::string dev_str = "plughw:3,0";  // ReSpeaker card 3

    // 先尝试用指定名找到设备对应的 card 号
    int card = -1;
    snd_ctl_t* ctl;
    int err = snd_card_next(&card);
    while (card >= 0) {
        char name[32];
        snprintf(name, sizeof(name), "hw:%d", card);
        if (snd_ctl_open(&ctl, name, 0) >= 0) {
            snd_ctl_card_info_t* info;
            snd_ctl_card_info_alloca(&info);
            if (snd_ctl_card_info(ctl, info) >= 0) {
                std::string card_name(snd_ctl_card_info_get_name(info));
                if (card_name.find("ReSpeaker") != std::string::npos) {
                    char hw[64];
                    snprintf(hw, sizeof(hw), "plughw:%d,0", card);
                    dev_str = hw;
                    snd_ctl_close(ctl);
                    break;
                }
            }
            snd_ctl_close(ctl);
        }
        snd_card_next(&card);
    }

    std::cout << "[AudioCapture] 打开 ALSA 设备: " << dev_str << std::endl;

    // 打开 PCM 采集设备（非阻塞模式）
    err = snd_pcm_open((snd_pcm_t**)&stream_, dev_str.c_str(),
                       SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        std::cerr << "[AudioCapture] snd_pcm_open 失败: "
                  << snd_strerror(err) << std::endl;
        return false;
    }

    snd_pcm_t* pcm = (snd_pcm_t*)stream_;

    // 设置硬件参数
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm, hw_params);

    // 交错模式、16bit LE、16000Hz、单声道
    snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_rate(pcm, hw_params, sample_rate_, 0);
    snd_pcm_hw_params_set_channels(pcm, hw_params, input_ch_);

    // 设置 buffer 大小
    snd_pcm_uframes_t buffer_size = frames_per_buffer_ * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw_params, &buffer_size);
    snd_pcm_hw_params_set_period_size_near(pcm, hw_params,
        (snd_pcm_uframes_t*)&frames_per_buffer_, nullptr);

    err = snd_pcm_hw_params(pcm, hw_params);
    if (err < 0) {
        std::cerr << "[AudioCapture] hw_params 失败: "
                  << snd_strerror(err) << std::endl;
        snd_pcm_close(pcm);
        stream_ = nullptr;
        return false;
    }

    // 准备采集
    err = snd_pcm_prepare(pcm);
    if (err < 0) {
        std::cerr << "[AudioCapture] prepare 失败: "
                  << snd_strerror(err) << std::endl;
        snd_pcm_close(pcm);
        stream_ = nullptr;
        return false;
    }

    running_ = true;
    return true;
}

void AudioCapture::stop() {
    if (!running_) return;
    snd_pcm_t* pcm = (snd_pcm_t*)stream_;
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    stream_ = nullptr;
    running_ = false;
}

bool AudioCapture::is_running() const {
    return running_;
}

int AudioCapture::read(std::vector<int16_t>& buffer) {
    if (!running_) return -1;
    buffer.resize(frames_per_buffer_);

    snd_pcm_t* pcm = (snd_pcm_t*)stream_;
    int err = snd_pcm_readi(pcm, buffer.data(), frames_per_buffer_);
    if (err == -EAGAIN) {
        return 0;  // 非阻塞模式下暂无数据
    }
    if (err < 0) {
        // xrun 恢复
        if (err == -EPIPE) {
            snd_pcm_prepare(pcm);
            return 0;
        }
        return -1;
    }
    return err;  // 实际帧数
}
