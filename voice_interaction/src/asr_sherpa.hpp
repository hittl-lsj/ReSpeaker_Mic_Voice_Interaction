#pragma once
#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include "sherpa-onnx/c-api/c-api.h"
}

/**
 * @brief sherpa-onnx 流式本地语音识别（替代 Vosk）
 *
 * 使用方式与旧 ASRVosk 完全一致：
 *   1. 构造时传入模型目录（含 encoder/decoder/joiner 三个 .onnx + tokens.txt）
 *   2. 循环调用 feed() 喂入音频帧
 *   3. 说话结束后调用 final_result() 拿最终文本（内部会 InputFinished 收尾）
 *
 * 说明：
 *   - 音频格式：16kHz / 16bit / mono / PCM（与 respeaker_driver 采集一致）
 *   - feed() 内部把 int16 转成 float 再交给 AcceptWaveform
 */
class ASRSherpa {
public:
    /**
     * @param model_dir   模型目录，例如
     *                    ".../sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20"
     * @param sample_rate 采样率（固定 16000）
     * @param num_threads onnxruntime 推理线程数（RK3588 建议 4，留核给 LLM/TTS）
     */
    ASRSherpa(const std::string& model_dir, float sample_rate = 16000,
              int num_threads = 4);
    ~ASRSherpa();

    /** 喂入一帧 PCM 16bit 音频。返回值当前未被调用方使用，恒返回 false。 */
    bool feed(const std::vector<int16_t>& samples);

    /** 收尾并返回最终识别文本（一次性，之后需 reset() 才能再用） */
    std::string final_result();

    /** 当前累计识别文本（非破坏性，边说边出） */
    std::string partial_result();

    /** 重置识别器（销毁旧 stream 重建，新一轮对话前调用） */
    void reset();

    /** 模型是否成功加载 */
    bool is_ready() const { return recognizer_ != nullptr; }

private:
    const SherpaOnnxOnlineRecognizer* recognizer_ = nullptr;
    const SherpaOnnxOnlineStream*     stream_ = nullptr;
    bool finished_ = false;   // 是否已 InputFinished（final_result 幂等）
};
