#pragma once

#include <rclcpp/rclcpp.hpp>

#include "llm_client.hpp"
#include "playback_manager.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ResponseStreamer {
public:
    struct Result {
        bool ok = false;             // LLM 请求及播报流程是否成功
        std::string provider;        // 实际使用的 provider 名称
        std::string reply;           // 完整文本回复
    };

    /** 绑定 LLM 客户端和播放管理器，二者由会话状态机持有。 */
    ResponseStreamer(const rclcpp::Logger& logger, LLMClient& llm,
                     PlaybackManager& playback);

    /**
     * @brief 流式请求 LLM，并按句子合成/播放回复
     * @param messages 当前会话上下文
     * @param should_continue 当前 generation 是否仍然有效
     */
    Result stream(const std::vector<ChatMessage>& messages,
                  const std::function<bool()>& should_continue);

    /** 播放一段文本，并在取消时停止播放。 */
    bool speak_text(const std::string& text,
                    const std::function<bool()>& should_continue);

    /** 取消当前 LLM 请求和音频播放。 */
    void cancel();

private:
    rclcpp::Logger logger_;
    LLMClient& llm_;
    PlaybackManager& playback_;
};
