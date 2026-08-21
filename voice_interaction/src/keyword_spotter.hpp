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
        std::string model_dir;
        std::string encoder;
        std::string decoder;
        std::string joiner;
        std::string tokens;
        std::string keywords_file;
        std::vector<std::string> keywords;
        int num_threads = 1;
        int max_active_paths = 4;
        int num_trailing_blanks = 1;
        float keywords_score = 3.0f;
        float keywords_threshold = 0.1f;
        std::string modeling_unit = "cjkchar";
    };

    KeywordSpotter(const rclcpp::Logger& logger, const Config& config);
    ~KeywordSpotter();

    bool is_ready() const { return spotter_ != nullptr && stream_ != nullptr; }

    void feed(const std::vector<int16_t>& samples);
    std::string poll();
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
