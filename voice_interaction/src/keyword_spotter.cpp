#include "keyword_spotter.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace {

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

}  // namespace

KeywordSpotter::KeywordSpotter(const rclcpp::Logger& logger,
                               const Config& config)
    : logger_(logger) {
    const std::string encoder = resolve_file(
        config.encoder, config.model_dir,
        {"encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx",
         "encoder-epoch-12-avg-2-chunk-16-left-64.onnx"});
    const std::string decoder = resolve_file(
        config.decoder, config.model_dir,
        {"decoder-epoch-12-avg-2-chunk-16-left-64.onnx",
         "decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx"});
    const std::string joiner = resolve_file(
        config.joiner, config.model_dir,
        {"joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx",
         "joiner-epoch-12-avg-2-chunk-16-left-64.onnx"});
    const std::string tokens =
        resolve_file(config.tokens, config.model_dir, {"tokens.txt"});

    if (encoder.empty() || decoder.empty() || joiner.empty() || tokens.empty()) {
        RCLCPP_WARN(logger_,
                    "KWS 模型文件不完整，回退到 ASR 唤醒。model_dir=%s",
                    config.model_dir.c_str());
        return;
    }

    std::string generated_keywords;
    const char* keywords_file = nullptr;
    const char* keywords_buf = nullptr;
    int32_t keywords_buf_size = 0;
    if (!config.keywords_file.empty() &&
        std::filesystem::exists(config.keywords_file)) {
        keywords_file = config.keywords_file.c_str();
    } else {
        RCLCPP_WARN(
            logger_,
            "KWS 关键词文件不存在，尝试用 wake_word_aliases 自动生成逐字关键词；"
            "默认 Wenetspeech KWS 为拼音 token 模型，建议配置 wake_words.txt");
        generated_keywords = build_keyword_buffer(config.keywords);
        if (generated_keywords.empty()) {
            RCLCPP_WARN(logger_, "KWS 没有可用的关键词，回退到 ASR 唤醒");
            return;
        }
        keywords_buf = generated_keywords.c_str();
        keywords_buf_size = static_cast<int32_t>(generated_keywords.size());
    }

    SherpaOnnxKeywordSpotterConfig kws_config{};
    kws_config.feat_config.sample_rate = 16000;
    kws_config.feat_config.feature_dim = 80;
    kws_config.model_config.transducer.encoder = encoder.c_str();
    kws_config.model_config.transducer.decoder = decoder.c_str();
    kws_config.model_config.transducer.joiner = joiner.c_str();
    kws_config.model_config.tokens = tokens.c_str();
    kws_config.model_config.num_threads = std::max(1, config.num_threads);
    kws_config.model_config.provider = "cpu";
    kws_config.model_config.modeling_unit = config.modeling_unit.c_str();
    kws_config.max_active_paths = std::max(1, config.max_active_paths);
    kws_config.num_trailing_blanks = std::max(1, config.num_trailing_blanks);
    kws_config.keywords_score = config.keywords_score;
    kws_config.keywords_threshold = config.keywords_threshold;
    kws_config.keywords_file = keywords_file;
    kws_config.keywords_buf = keywords_buf;
    kws_config.keywords_buf_size = keywords_buf_size;

    spotter_ = SherpaOnnxCreateKeywordSpotter(&kws_config);
    if (!spotter_) {
        RCLCPP_WARN(logger_, "KWS 模型加载失败，回退到 ASR 唤醒");
        return;
    }

    stream_ = SherpaOnnxCreateKeywordStream(spotter_);
    if (!stream_) {
        RCLCPP_WARN(logger_, "KWS 流创建失败，回退到 ASR 唤醒");
        SherpaOnnxDestroyKeywordSpotter(spotter_);
        spotter_ = nullptr;
        return;
    }

    RCLCPP_INFO(logger_, "轻量 KWS 已启用: %s", config.model_dir.c_str());
}

KeywordSpotter::~KeywordSpotter() {
    if (stream_) SherpaOnnxDestroyOnlineStream(stream_);
    if (spotter_) SherpaOnnxDestroyKeywordSpotter(spotter_);
}

void KeywordSpotter::feed(const std::vector<int16_t>& samples) {
    if (!is_ready() || samples.empty()) return;

    std::vector<float> waveform(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        waveform[i] = samples[i] / 32768.0f;

    SherpaOnnxOnlineStreamAcceptWaveform(
        stream_, 16000, waveform.data(),
        static_cast<int32_t>(waveform.size()));
    while (SherpaOnnxIsKeywordStreamReady(spotter_, stream_))
        SherpaOnnxDecodeKeywordStream(spotter_, stream_);
}

std::string KeywordSpotter::poll() {
    if (!is_ready()) return "";

    const SherpaOnnxKeywordResult* result =
        SherpaOnnxGetKeywordResult(spotter_, stream_);
    if (!result) return "";

    std::string keyword;
    if (result->keyword) keyword = result->keyword;
    SherpaOnnxDestroyKeywordResult(result);
    if (keyword.empty()) return "";

    SherpaOnnxResetKeywordStream(spotter_, stream_);
    return keyword;
}

void KeywordSpotter::reset() {
    if (is_ready()) SherpaOnnxResetKeywordStream(spotter_, stream_);
}

std::string KeywordSpotter::resolve_file(
    const std::string& explicit_path, const std::string& model_dir,
    const std::vector<std::string>& names) {
    if (!explicit_path.empty() &&
        std::filesystem::exists(explicit_path))
        return explicit_path;

    for (const auto& name : names) {
        const std::string path = join_path(model_dir, name);
        if (std::filesystem::exists(path)) return path;
    }
    return "";
}

std::string KeywordSpotter::build_keyword_buffer(
    const std::vector<std::string>& keywords) {
    std::ostringstream output;
    for (const auto& keyword : keywords) {
        if (keyword.empty()) continue;
        output << utf8_tokenize(keyword) << " @" << keyword << "\n";
    }
    return output.str();
}

std::string KeywordSpotter::utf8_tokenize(const std::string& text) {
    std::ostringstream output;
    bool first = true;

    for (size_t i = 0; i < text.size();) {
        size_t width = 1;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0x80) == 0) {
            width = 1;
        } else if ((c & 0xE0) == 0xC0) {
            width = 2;
        } else if ((c & 0xF0) == 0xE0) {
            width = 3;
        } else if ((c & 0xF8) == 0xF0) {
            width = 4;
        }
        if (i + width > text.size()) width = 1;

        if (!first) output << ' ';
        output.write(text.data() + i, static_cast<std::streamsize>(width));
        first = false;
        i += width;
    }
    return output.str();
}
