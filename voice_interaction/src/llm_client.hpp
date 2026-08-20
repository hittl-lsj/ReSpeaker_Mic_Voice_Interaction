#pragma once
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>
#include "http_client.hpp"

struct ChatMessage {
    std::string role;      // "system" / "user" / "assistant"
    std::string content;
};

struct LLMProvider {
    std::string name;      // 显示名
    std::string type;      // "openai" 或 "anthropic"
    std::string base_url;  // API 地址
    std::string api_key;   // 密钥
    std::string model;     // 模型名
    int  consecutive_failures = 0;      // 连续失败次数
    int64_t skip_until_ms = 0;          // 此时间戳之前跳过该 provider
};

class LLMClient {
public:
    LLMClient();

    void add_provider(const LLMProvider& p);

    std::string chat(const std::vector<ChatMessage>& messages,
                     std::string& used_name);

    bool chat_stream(const std::vector<ChatMessage>& messages,
                     std::function<void(const std::string&)> on_token,
                     std::string& used_name);

    bool has_any_provider() const { return !providers_.empty(); }

    /** 设置 HTTP 代理（空 = 直连），透传给底层 HttpClient */
    void set_proxy(const std::string& proxy);

    /** 重置所有 provider 的失败计数（网络恢复后调用） */
    void reset_all();

    void cancel_current() { cancel_requested_.store(true); }
    void reset_cancel() { cancel_requested_.store(false); }

private:
    HttpClient http_;
    std::vector<LLMProvider> providers_;
    std::atomic<bool> cancel_requested_{false};

    static constexpr int     MAX_FAILURES = 2;     // 连续失败几次后跳过
    static constexpr int64_t RETRY_MS    = 30000;  // 30 秒后重试

    /** 是否应该尝试此 provider（没被冷却 或 到了重试时间） */
    bool should_try(LLMProvider& p);
    void mark_success(LLMProvider& p);
    void mark_failure(LLMProvider& p);
    int64_t now_ms();

    // OpenAI 兼容格式
    std::string build_openai_body(const std::vector<ChatMessage>& msgs,
                                  const LLMProvider& p, bool stream);
    std::string parse_openai(const std::string& json);

    // Anthropic 原生格式
    std::string build_anthropic_body(const std::vector<ChatMessage>& msgs,
                                     const LLMProvider& p, bool stream);
    std::string parse_anthropic(const std::string& json);
};
