#include "asr_vosk.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>

ASRVosk::ASRVosk(const std::string& model_path, float sample_rate) {
    // 1. 加载语音模型（包含声学模型、语言模型、词典）
    model_ = vosk_model_new(model_path.c_str());
    if (!model_) {
        std::cerr << "[ASR Vosk] 模型加载失败: " << model_path << std::endl;
        rec_ = nullptr;
        return;
    }

    // 2. 创建识别器
    rec_ = vosk_recognizer_new(model_, sample_rate);
    if (!rec_) {
        std::cerr << "[ASR Vosk] 识别器创建失败" << std::endl;
    }
}

ASRVosk::~ASRVosk() {
    if (rec_) vosk_recognizer_free(rec_);
    if (model_) vosk_model_free(model_);
}

bool ASRVosk::feed(const std::vector<int16_t>& samples) {
    if (!rec_) return false;
    // accept_waveform 返回 1 表示检测到一句话结束
    return vosk_recognizer_accept_waveform(rec_,
        (const char*)samples.data(), samples.size() * sizeof(int16_t)) != 0;
}

std::string ASRVosk::final_result() {
    if (!rec_) return "";
    // result() 返回 JSON: {"text": "你好世界"}
    const char* json = vosk_recognizer_result(rec_);
    if (!json) return "";

    // 从 JSON 中提取 "text" 字段（简单字符串解析，不引入 JSON 库）
    std::string s(json);
    auto start = s.find("\"text\" : \"");
    if (start == std::string::npos) return "";
    start += 10;  // 跳过 "text" : "
    auto end = s.find("\"", start);
    if (end == std::string::npos) return "";
    std::string text = s.substr(start, end - start);
    // 去掉空格（Vosk 中文模型输出分词格式，中文不需要空格）
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    return text;
}

std::string ASRVosk::partial_result() {
    if (!rec_) return "";
    const char* json = vosk_recognizer_partial_result(rec_);
    if (!json) return "";

    std::string s(json);
    auto start = s.find("\"partial\" : \"");
    if (start == std::string::npos) return "";
    start += 14;
    auto end = s.find("\"", start);
    if (end == std::string::npos) return "";
    std::string text = s.substr(start, end - start);
    // 去掉空格（Vosk 中文模型输出分词格式，中文不需要空格）
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    return text;
}

void ASRVosk::reset() {
    if (rec_) vosk_recognizer_reset(rec_);
}
