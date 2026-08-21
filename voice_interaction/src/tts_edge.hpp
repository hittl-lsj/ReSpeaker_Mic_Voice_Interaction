#pragma once
#include <string>
#include <atomic>
#include <csignal>
#include <mutex>

class TTSEdge {
public:
    /**
     * @param voice  TTS 发音人
     * @param audio_dev 音频输出设备；为空或 "default" 使用系统默认输出
     */
    TTSEdge(const std::string& voice = "zh-CN-XiaoxiaoNeural",
            const std::string& audio_dev = "");  // 空 = 系统默认
    ~TTSEdge();

    /** 整段合成+播放（阻塞） */
    bool speak(const std::string& text);

    /**
     * @brief 播放一句（不阻塞）。
     * 调用后立即返回，ffplay 在后台播放。
     * 配合 stream_speak() 使用可实现流式 TTS。
     */
    bool speak_async(const std::string& text);

    /**
     * @brief 仅合成到指定 mp3，不播放
     * @param text 待合成文本
     * @param mp3_path 输出 mp3 路径
     */
    bool synthesize(const std::string& text, const std::string& mp3_path);

    /**
     * @brief 等待当前正在播放的音频结束。
     * 如果没在播放则立即返回。返回后可以安全地播下一句。
     */
    void wait_done();

    /** 立刻停止播放 */
    void stop();

    /** 是否正在播放 */
    bool is_playing();

    /** 修改后续 edge-tts 使用的发音人。 */
    void set_voice(const std::string& voice) { voice_ = voice; }

private:
    std::string voice_;
    std::string audio_dev_;   // 输出设备，空=系统默认
    std::string tmp_mp3_;
    std::string tmp_txt_;
    std::atomic<int> ffplay_pid_{0};
    std::atomic<bool> playing_{false};
    mutable std::mutex process_mutex_;

    /** 杀掉 ffplay 子进程 */
    void kill_ffplay();
};
