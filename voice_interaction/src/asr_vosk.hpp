#pragma once
#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include <vosk_api.h>
}

/**
 * @brief Vosk 本地离线语音识别
 *
 * 使用方式：
 *   1. 构造时传入模型路径
 *   2. 循环调用 feed() 喂入音频帧
 *   3. 说话结束后调用 final_result() 拿最终文本
 */
class ASRVosk {
public:
    /**
     * @param model_path Vosk 模型目录
     * @param sample_rate 输入音频采样率，通常为 16000 Hz
     */
    ASRVosk(const std::string& model_path, float sample_rate = 16000);
    ~ASRVosk();

    /** 喂入一帧 PCM 16bit 音频 */
    bool feed(const std::vector<int16_t>& samples);

    /** 获取最终识别结果（说话结束后调用） */
    std::string final_result();

    /** 获取中间结果（边说边出的部分文本） */
    std::string partial_result();

    /** 重置识别器（新一轮对话前调用） */
    void reset();

    /** 模型是否成功加载 */
    bool is_ready() const { return model_ != nullptr; }

private:
    VoskModel*       model_;
    VoskRecognizer*  rec_;
};

// Vosk API 封装说明：
//
// vosk_model_new(path)     → 加载模型文件
// vosk_recognizer_new(model, sample_rate)  → 创建识别器
// vosk_recognizer_accept_waveform(rec, samples, n)  → 喂音频帧，返回 1 表示这句话结束
// vosk_recognizer_result(rec)   → 获取这句完整结果（JSON 格式）
// vosk_recognizer_partial_result(rec)  → 获取中间结果
// vosk_recognizer_reset(rec)    → 重置，准备下一句
//
// 返回值都是 JSON 字符串，解析 "text" 字段就是识别文本。
// 例如: {"text": "你好世界"}
