#include "asr_sherpa.hpp"
#include <iostream>

// 注意：下面的 C API 函数/结构体字段名已对照 sherpa-onnx v1.13.6 的
//   sherpa-onnx/c-api/c-api.h 核对过。
// 若以后升级 sherpa-onnx 版本后编译报某个名字找不到，先在该头文件里核对。

ASRSherpa::ASRSherpa(const std::string& model_dir, float sample_rate,
                     int num_threads) {
    if (model_dir.empty()) return;

    // ---- 拼模型文件路径（2023-02 系列 zipformer 的文件名）----
    const std::string encoder = model_dir + "/encoder-epoch-99-avg-1.onnx";
    const std::string decoder = model_dir + "/decoder-epoch-99-avg-1.onnx";
    const std::string joiner  = model_dir + "/joiner-epoch-99-avg-1.onnx";
    const std::string tokens  = model_dir + "/tokens.txt";

    // ---- 组装配置 ----
    SherpaOnnxOnlineTransducerModelConfig transducer{};
    transducer.encoder = encoder.c_str();
    transducer.decoder = decoder.c_str();
    transducer.joiner  = joiner.c_str();

    SherpaOnnxOnlineModelConfig model{};
    model.transducer = transducer;
    model.tokens     = tokens.c_str();
    model.num_threads = num_threads;
    model.provider    = "cpu";       // 本次 CPU；将来 NPU 改 "rknn"（需 rknn 构建+模型）
    // model.model_type 留空，sherpa-onnx 会根据 transducer 字段自动识别。
    // 若报错要求显式指定 model_type，填 "zipformer"。

    SherpaOnnxFeatureConfig feat{};
    feat.sample_rate = static_cast<int32_t>(sample_rate);  // 16000
    feat.feature_dim = 80;

    SherpaOnnxOnlineRecognizerConfig cfg{};
    cfg.model_config   = model;
    cfg.feat_config    = feat;
    cfg.decoding_method = "greedy_search";
    cfg.enable_endpoint = 0;   // endpoint 检测未使用（feed 返回值被忽略），关闭省事

    recognizer_ = SherpaOnnxCreateOnlineRecognizer(&cfg);
    if (!recognizer_) {
        std::cerr << "[ASR sherpa] 模型加载失败: " << model_dir << std::endl;
        return;
    }

    stream_ = SherpaOnnxCreateOnlineStream(recognizer_);
    if (!stream_) {
        std::cerr << "[ASR sherpa] 创建识别流失败" << std::endl;
        SherpaOnnxDestroyOnlineRecognizer(recognizer_);
        recognizer_ = nullptr;
    }
}

ASRSherpa::~ASRSherpa() {
    if (stream_)     SherpaOnnxDestroyOnlineStream(stream_);
    if (recognizer_) SherpaOnnxDestroyOnlineRecognizer(recognizer_);
    stream_ = nullptr;
    recognizer_ = nullptr;
}

bool ASRSherpa::feed(const std::vector<int16_t>& samples) {
    if (!stream_ || samples.empty()) return false;

    // int16 -> float（sherpa-onnx 吃 float 波形）
    std::vector<float> f(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        f[i] = samples[i] / 32768.0f;

    SherpaOnnxOnlineStreamAcceptWaveform(stream_, 16000, f.data(),
                                         static_cast<int32_t>(f.size()));

    // 解到没有 ready 帧为止（增量出 partial 结果）
    while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_))
        SherpaOnnxDecodeOnlineStream(recognizer_, stream_);

    // 原 Vosk 的 feed() 返回"是否检测到句末"，但本项目两处调用点都忽略了返回值，
    // 故这里不做 endpoint 检测，恒返回 false。
    return false;
}

std::string ASRSherpa::partial_result() {
    if (!recognizer_ || !stream_) return "";
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
    return (r && r->text) ? std::string(r->text) : std::string();
}

std::string ASRSherpa::final_result() {
    if (!recognizer_ || !stream_) return "";

    // 收尾：告知无更多输入，解完剩余帧（幂等）
    if (!finished_) {
        SherpaOnnxOnlineStreamInputFinished(stream_);
        finished_ = true;
        while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_))
            SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
    }

    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
    return (r && r->text) ? std::string(r->text) : std::string();
}

void ASRSherpa::reset() {
    if (!recognizer_) return;
    // 销毁旧 stream 重建新 stream（InputFinished 之后的流不能直接复用）
    if (stream_) {
        SherpaOnnxDestroyOnlineStream(stream_);
        stream_ = nullptr;
    }
    stream_ = SherpaOnnxCreateOnlineStream(recognizer_);
    finished_ = false;
}
