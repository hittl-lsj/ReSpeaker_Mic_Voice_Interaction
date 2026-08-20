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
        bool ok = false;
        std::string provider;
        std::string reply;
    };

    ResponseStreamer(const rclcpp::Logger& logger, LLMClient& llm,
                     PlaybackManager& playback);

    Result stream(const std::vector<ChatMessage>& messages,
                  const std::function<bool()>& should_continue);

    bool speak_text(const std::string& text,
                    const std::function<bool()>& should_continue);
    void cancel();

private:
    rclcpp::Logger logger_;
    LLMClient& llm_;
    PlaybackManager& playback_;
};
