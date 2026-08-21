#pragma once
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <atomic>
#include "http_client.hpp"

struct ChatMessage {
    std::string role;      // system、user 或 assistant
    std::string content;   // 消息正文
};

struct LLMProvider {
    std::string name;      // 日志中显示的 provider 名称
    std::string type;      // openai 或 anthropic
    std::string base_url;  // API 根地址
    std::string api_key;   // API 密钥；本地 Ollama 可填任意占位值
    std::string model;     // 远端模型名
    int consecutive_failures = 0;  // 连续失败次数
    int64_t skip_until_ms = 0;  // 冷却截止时间，使用 steady_clock 毫秒
};

class LLMClient {
public:
    LLMClient();

    /** 按优先级追加一个 LLM provider。 */
    void add_provider(const LLMProvider& p);

    /**
     * @param messages 完整对话上下文
     * @param used_name 输出实际成功的 provider 名称
     */
    std::string chat(const std::vector<ChatMessage>& messages,
                     std::string& used_name);

    /**
     * @param messages 完整对话上下文
     * @param on_token 收到一段增量文本时调用
     * @param used_name 输出实际成功的 provider 名称
     * @return 是否成功完成流式请求
     */
    bool chat_stream(const std::vector<ChatMessage>& messages,
                     std::function<void(const std::string&)> on_token,
                     std::string& used_name);

    /** 是否至少注册了一个 provider。 */
    bool has_any_provider() const { return !providers_.empty(); }

    /** @param proxy HTTP 代理地址；空字符串表示直连 */
    void set_proxy(const std::string& proxy);

    /** 重置所有 provider 的失败计数，网络恢复后调用。 */
    void reset_all();

    /** 请求取消当前 HTTP/流式请求，供插话和关机使用。 */
    void cancel_current() { cancel_requested_.store(true); }

    /** 清除取消标记，允许下一轮请求继续执行。 */
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
