#include "llm_client.hpp"
#include "utils.hpp"
#include <iostream>
#include <sstream>

LLMClient::LLMClient() {}

void LLMClient::set_proxy(const std::string& proxy) {
    http_.set_proxy(proxy);
}

void LLMClient::reset_all() {
    for (auto& p : providers_) {
        p.consecutive_failures = 0;
        p.skip_until_ms = 0;
    }
}

bool LLMClient::should_try(LLMProvider& p) {
    if (p.consecutive_failures < MAX_FAILURES) return true;
    if (now_ms() >= p.skip_until_ms) {
        p.consecutive_failures = 0;  // 冷却结束，给一次重试机会
        return true;
    }
    return false;
}

void LLMClient::mark_success(LLMProvider& p) {
    p.consecutive_failures = 0;
    p.skip_until_ms = 0;
}

void LLMClient::mark_failure(LLMProvider& p) {
    p.consecutive_failures++;
    if (p.consecutive_failures >= MAX_FAILURES)
        p.skip_until_ms = now_ms() + RETRY_MS;
}

int64_t LLMClient::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LLMClient::add_provider(const LLMProvider& p) {
    providers_.push_back(p);
}

// ====== OpenAI 兼容格式 ======
// POST /v1/chat/completions
// {"model":"x","messages":[...],"stream":false}
// → {"choices":[{"message":{"content":"..."}}]}
std::string LLMClient::build_openai_body(const std::vector<ChatMessage>& msgs,
                                          const LLMProvider& p, bool stream) {
    std::ostringstream oss;
    oss << "{\"model\":\"" << p.model << "\",\"messages\":[";
    for (size_t i = 0; i < msgs.size(); i++) {
        if (i > 0) oss << ",";
        oss << "{\"role\":\"" << msgs[i].role << "\","
            << "\"content\":\"" << escape_json(msgs[i].content) << "\"}";
    }
    oss << "],\"stream\":" << (stream ? "true" : "false") << "}";
    return oss.str();
}

std::string LLMClient::parse_openai(const std::string& json) {
    auto p = json.find("\"content\":\"");
    if (p == std::string::npos) return "";
    p += 11;
    auto end = json.find("\"", p);
    if (end == std::string::npos) return "";
    return json.substr(p, end - p);
}

// ====== Anthropic 原生格式 ======
// POST /v1/messages
// Header: x-api-key, anthropic-version: 2023-06-01
// {"model":"x","max_tokens":1024,"system":"你是助手","messages":[...]}
// → {"content":[{"type":"text","text":"..."}]}
std::string LLMClient::build_anthropic_body(const std::vector<ChatMessage>& msgs,
                                              const LLMProvider& p, bool stream) {
    // Anthropic system prompt 是独立字段，不是 messages 里的一条
    std::string system_text;
    std::ostringstream msg_arr;
    msg_arr << "[";
    bool first = true;
    for (auto& m : msgs) {
        if (m.role == "system") {
            system_text = m.content;
            continue;
        }
        if (!first) msg_arr << ",";
        first = false;
        msg_arr << "{\"role\":\"" << m.role << "\","
                << "\"content\":\"" << escape_json(m.content) << "\"}";
    }
    msg_arr << "]";

    std::ostringstream oss;
    oss << "{\"model\":\"" << p.model << "\","
        << "\"max_tokens\":1024,";
    if (!system_text.empty())
        oss << "\"system\":\"" << escape_json(system_text) << "\",";
    oss << "\"messages\":" << msg_arr.str();
    if (stream) oss << ",\"stream\":true";
    oss << "}";
    return oss.str();
}

std::string LLMClient::parse_anthropic(const std::string& json) {
    // {"content":[{"type":"text","text":"你好！"}]}
    auto p = json.find("\"text\":\"");
    if (p == std::string::npos) return "";
    p += 8;
    auto end = json.find("\"", p);
    if (end == std::string::npos) return "";
    return json.substr(p, end - p);
}

// ====== chat ======
std::string LLMClient::chat(const std::vector<ChatMessage>& messages,
                             std::string& used_name) {
    for (auto& p : providers_) {
        if (!should_try(p)) {
            std::cerr << "[LLM] " << p.name << " 已冷却，跳过" << std::endl;
            continue;
        }

        std::string body, url, resp;
        std::map<std::string, std::string> headers;

        if (p.type == "anthropic") {
            body = build_anthropic_body(messages, p, false);
            url = p.base_url + "/messages";
            headers = {{"x-api-key", p.api_key},
                       {"anthropic-version", "2023-06-01"},
                       {"Content-Type", "application/json"}};
            resp = http_.post(url, body, headers);
            std::string answer = parse_anthropic(resp);
            if (!answer.empty()) {
                mark_success(p);
                used_name = p.name;
                return answer;
            }
        } else {
            body = build_openai_body(messages, p, false);
            url = p.base_url + "/chat/completions";
            headers = {{"Authorization", "Bearer " + p.api_key},
                       {"Content-Type", "application/json"}};
            resp = http_.post(url, body, headers);
            std::string answer = parse_openai(resp);
            if (!answer.empty()) {
                mark_success(p);
                used_name = p.name;
                return answer;
            }
        }
        mark_failure(p);
        std::cerr << "[LLM] " << p.name << " 不可用" << std::endl;
    }
    used_name = "none";
    return "";
}

// ====== chat_stream ======
bool LLMClient::chat_stream(const std::vector<ChatMessage>& messages,
                             std::function<void(const std::string&)> on_token,
                             std::string& used_name) {
    for (auto& p : providers_) {
        if (cancel_requested_.load()) return false;
        if (!should_try(p)) {
            std::cerr << "[LLM stream] " << p.name << " 已冷却，跳过" << std::endl;
            continue;
        }

        std::string body, url;
        std::map<std::string, std::string> headers;
        bool is_anthropic = (p.type == "anthropic");

        if (is_anthropic) {
            body = build_anthropic_body(messages, p, true);
            url = p.base_url + "/messages";
            headers = {{"x-api-key", p.api_key},
                       {"anthropic-version", "2023-06-01"},
                       {"Content-Type", "application/json"}};
        } else {
            body = build_openai_body(messages, p, true);
            url = p.base_url + "/chat/completions";
            headers = {{"Authorization", "Bearer " + p.api_key},
                       {"Content-Type", "application/json"}};
        }

        bool got_any = false;
        http_.post_stream(url, body,
            [&](const std::string& line) {
                if (cancel_requested_.load()) return;
                if (line.empty()) return;
                if (line == "data: [DONE]") return;
                std::string token;
                if (is_anthropic) {
                    token = parse_anthropic(line);
                } else {
                    if (line.rfind("data: ", 0) != 0) return;
                    token = parse_openai(line.substr(6));
                }
                if (!token.empty()) { got_any = true; on_token(token); }
            }, headers, [this]() { return cancel_requested_.load(); });

        if (cancel_requested_.load()) return false;

        if (got_any) {
            mark_success(p);
            used_name = p.name;
            return true;
        }
        mark_failure(p);
        std::cerr << "[LLM stream] " << p.name << " 不可用" << std::endl;
    }
    used_name = "none";
    return false;
}
