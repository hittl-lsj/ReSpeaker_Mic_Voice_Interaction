#include "response_streamer.hpp"

#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

ResponseStreamer::ResponseStreamer(const rclcpp::Logger& logger, LLMClient& llm,
                                   PlaybackManager& playback)
    : logger_(logger), llm_(llm), playback_(playback) {}

ResponseStreamer::Result ResponseStreamer::stream(
    const std::vector<ChatMessage>& messages,
    const std::function<bool()>& should_continue) {
    Result result;
    llm_.reset_cancel();

    struct SynthesisJob {
        std::string text;
        int index = 0;
    };
    struct PlayItem {
        std::string text;
        std::string file;
    };

    std::deque<SynthesisJob> synthesis_queue;
    std::deque<PlayItem> play_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    bool input_done = false;
    bool synthesis_done = false;

    auto clear_files = [](std::deque<PlayItem>& queue) {
        for (const auto& item : queue)
            if (!item.file.empty()) std::remove(item.file.c_str());
        queue.clear();
    };

    std::thread synthesizer([&]() {
        while (true) {
            SynthesisJob job;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
                    return !synthesis_queue.empty() || input_done ||
                           !should_continue();
                });
                if (!should_continue()) {
                    synthesis_queue.clear();
                    break;
                }
                if (synthesis_queue.empty() && input_done) break;
                if (synthesis_queue.empty()) continue;
                job = std::move(synthesis_queue.front());
                synthesis_queue.pop_front();
            }

            std::string file;
            playback_.synthesize(job.text, file);
            if (!should_continue()) {
                if (!file.empty()) std::remove(file.c_str());
                break;
            }

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                play_queue.push_back({std::move(job.text), std::move(file)});
            }
            queue_cv.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            synthesis_done = true;
        }
        queue_cv.notify_all();
    });

    std::thread speaker([&]() {
        while (true) {
            PlayItem item;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                queue_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
                    return !play_queue.empty() || synthesis_done ||
                           !should_continue();
                });
                if (!should_continue()) {
                    clear_files(play_queue);
                    break;
                }
                if (play_queue.empty() && synthesis_done) break;
                if (play_queue.empty()) continue;
                item = std::move(play_queue.front());
                play_queue.pop_front();
            }

            if (!item.file.empty()) {
                playback_.play_file(item.file, should_continue);
                std::remove(item.file.c_str());
            } else {
                playback_.speak(item.text, should_continue);
            }
        }
    });

    std::string full_reply;
    std::string pending;
    int sentence_index = 0;
    auto enqueue_sentence = [&](std::string sentence) {
        if (sentence.empty() || !should_continue()) return;
        std::lock_guard<std::mutex> lock(queue_mutex);
        synthesis_queue.push_back({std::move(sentence), sentence_index++});
        queue_cv.notify_all();
    };

    const bool ok = llm_.chat_stream(
        messages,
        [&](const std::string& token) {
            if (!should_continue()) return;
            full_reply += token;
            pending += token;

            const bool boundary =
                token.find('!') != std::string::npos ||
                token.find('?') != std::string::npos ||
                token.find('\n') != std::string::npos ||
                token.find("。") != std::string::npos ||
                token.find("！") != std::string::npos ||
                token.find("？") != std::string::npos ||
                pending.size() >= 60;
            if (boundary) {
                std::string sentence = std::move(pending);
                pending.clear();
                enqueue_sentence(std::move(sentence));
            }
        },
        result.provider);

    if (!pending.empty() && should_continue())
        enqueue_sentence(std::move(pending));

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        input_done = true;
    }
    queue_cv.notify_all();

    synthesizer.join();
    speaker.join();

    if (!should_continue()) return result;

    result.ok = ok;
    result.reply = full_reply;
    if (result.reply.empty()) {
        result.reply = "抱歉，无法回答";
        playback_.speak(result.reply, should_continue);
    }
    return result;
}

bool ResponseStreamer::speak_text(
    const std::string& text,
    const std::function<bool()>& should_continue) {
    if (!playback_.speak_async(text)) return false;
    return playback_.wait_until_done(should_continue);
}

void ResponseStreamer::cancel() {
    llm_.cancel_current();
    playback_.stop();
}
