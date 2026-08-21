#pragma once

#include <rclcpp/rclcpp.hpp>

extern "C" {
#include "sherpa-onnx/c-api/c-api.h"
}

#include <cstdint>
#include <string>
#include <vector>

class KeywordSpotter {
public:
    struct Config {
        std::string model_dir;       // 模型目录，未显式指定文件时从这里解析
        std::string encoder;         // 可选：显式指定 encoder.onnx
        std::string decoder;         // 可选：显式指定 decoder.onnx
        std::string joiner;          // 可选：显式指定 joiner.onnx
        std::string tokens;          // 可选：显式指定 tokens.txt
        std::string keywords_file;   // 关键词文件；优先于自动生成的关键词
        std::vector<std::string> keywords;  // 无关键词文件时使用的显式词列表
        int num_threads = 1;         // ONNX Runtime 推理线程数
        int max_active_paths = 4;    // modified beam search 的候选路径数
        int num_trailing_blanks = 1; // 关键词后需要的 blank 数
        float keywords_score = 3.0f; // 关键词 token 的加分
        float keywords_threshold = 0.1f;  // 触发关键词的声学阈值
        std::string modeling_unit = "cjkchar";  // 模型 token 单元
    };

    /** 根据模型配置创建流式关键词检测器。 */
    KeywordSpotter(const rclcpp::Logger& logger, const Config& config);
    ~KeywordSpotter();

    /** 模型和内部 stream 是否都已成功创建。 */
    bool is_ready() const { return spotter_ != nullptr && stream_ != nullptr; }

    /** 向 KWS stream 喂入一批 16 kHz、16-bit、mono PCM 音频。 */
    void feed(const std::vector<int16_t>& samples);

    /** 获取并消费最近一次命中的关键词；没有命中时返回空字符串。 */
    std::string poll();

    /** 清空当前 KWS stream，准备检测下一次关键词。 */
    void reset();

private:
    rclcpp::Logger logger_;
    const SherpaOnnxKeywordSpotter* spotter_ = nullptr;
    const SherpaOnnxOnlineStream* stream_ = nullptr;

    static std::string resolve_file(const std::string& explicit_path,
                                    const std::string& model_dir,
                                    const std::vector<std::string>& names);
    static std::string build_keyword_buffer(
        const std::vector<std::string>& keywords);
    static std::string utf8_tokenize(const std::string& text);
};
