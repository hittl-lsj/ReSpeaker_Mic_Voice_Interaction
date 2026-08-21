#include "playback_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {

bool use_audio_device(const std::string& device) {
    return !device.empty() && device != "default";
}

}  // namespace

PlaybackManager::PlaybackManager(const rclcpp::Logger& logger,
                                 const Config& config)
    : logger_(logger), config_(config) {
    edge_ = std::make_unique<TTSEdge>(config_.tts_voice, config_.tts_device);
    piper_ = std::make_unique<TTSPiper>(config_.piper_model, config_.tts_device);
}

PlaybackManager::~PlaybackManager() {
    stop();
}

bool PlaybackManager::synthesize(const std::string& text,
                                 std::string& output_path) {
    const std::string spoken_text = sanitize_tts_text(text);
    if (spoken_text.empty()) return false;

    std::lock_guard<std::mutex> lock(synthesis_mutex_);

    const std::string mp3 = unique_path(".mp3");
    if (try_edge_tts() && edge_->synthesize(spoken_text, mp3)) {
        edge_tts_failures_ = 0;
        output_path = mp3;
        return true;
    }

    ++edge_tts_failures_;
    if (edge_tts_failures_ >= kEdgeMaxFailures)
        edge_tts_skip_until_ms_ = now_ms() + kEdgeRetryMs;

    const std::string wav = unique_path(".wav");
    if (piper_->synthesize(spoken_text, wav)) {
        std::remove(mp3.c_str());
        output_path = wav;
        return true;
    }

    std::remove(mp3.c_str());
    std::remove(wav.c_str());
    output_path.clear();
    return false;
}

bool PlaybackManager::speak(const std::string& text,
                             const std::function<bool()>& should_continue) {
    const std::string spoken_text = sanitize_tts_text(text);
    if (spoken_text.empty()) return false;
    if (should_continue && !should_continue()) return false;

    std::lock_guard<std::mutex> lock(synthesis_mutex_);
    if (try_edge_tts() && edge_->speak(spoken_text)) {
        edge_tts_failures_ = 0;
        return true;
    }

    ++edge_tts_failures_;
    if (edge_tts_failures_ >= kEdgeMaxFailures)
        edge_tts_skip_until_ms_ = now_ms() + kEdgeRetryMs;

    if (piper_->speak(spoken_text)) return true;
    return espeak_speak(spoken_text, should_continue);
}

bool PlaybackManager::speak_async(const std::string& text) {
    const std::string spoken_text = sanitize_tts_text(text);
    if (spoken_text.empty()) return false;

    std::lock_guard<std::mutex> lock(synthesis_mutex_);
    if (try_edge_tts() && edge_->speak_async(spoken_text)) {
        edge_tts_failures_ = 0;
        return true;
    }

    ++edge_tts_failures_;
    if (edge_tts_failures_ >= kEdgeMaxFailures)
        edge_tts_skip_until_ms_ = now_ms() + kEdgeRetryMs;

    if (piper_->speak_async(spoken_text)) return true;
    return espeak_speak(spoken_text, {});
}

bool PlaybackManager::wait_until_done(
    const std::function<bool()>& should_continue) {
    while (is_playing()) {
        if (should_continue && !should_continue()) {
            stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !should_continue || should_continue();
}

bool PlaybackManager::play_file(
    const std::string& file,
    const std::function<bool()>& should_continue) {
    if (file.empty() || (should_continue && !should_continue()))
        return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (use_audio_device(config_.tts_device))
            setenv("AUDIODEV", config_.tts_device.c_str(), 1);
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
               "-loglevel", "quiet", "-af", "channelmap=0-0|0-1",
               file.c_str(), nullptr);
        _exit(1);
    }

    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        pipeline_player_pid_ = pid;
    }
    if (should_continue && !should_continue()) stop();

    int status = 0;
    while (true) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || result < 0) break;
        if (should_continue && !should_continue()) stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        if (pipeline_player_pid_ == pid) pipeline_player_pid_ = 0;
    }

    return (!should_continue || should_continue()) &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void PlaybackManager::stop() {
    if (edge_) edge_->stop();
    if (piper_) piper_->stop();

    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    if (pipeline_player_pid_ > 0)
        kill(pipeline_player_pid_, SIGTERM);
}

bool PlaybackManager::is_playing() const {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    return pipeline_player_pid_ > 0 ||
           (edge_ && edge_->is_playing()) ||
           (piper_ && piper_->is_playing());
}

bool PlaybackManager::try_edge_tts() {
    if (edge_tts_failures_ < kEdgeMaxFailures) return true;
    if (now_ms() >= edge_tts_skip_until_ms_) {
        edge_tts_failures_ = 0;
        return true;
    }
    return false;
}

bool PlaybackManager::espeak_speak(
    const std::string& text,
    const std::function<bool()>& should_continue) {
    if (should_continue && !should_continue()) return false;

    const std::string wav = unique_path(".wav");
    const std::string command = "espeak-ng " + escape_shell(text) +
                                " -v zh -w " + escape_shell(wav) +
                                " 2>/dev/null";
    if (system(command.c_str()) != 0) {
        RCLCPP_WARN(logger_, "espeak-ng 合成失败");
        std::remove(wav.c_str());
        return false;
    }

    const bool ok = play_file(wav, should_continue);
    std::remove(wav.c_str());
    return ok;
}

std::string PlaybackManager::unique_path(const std::string& extension) {
    return "/tmp/voice_assistant_" + std::to_string(getpid()) + "_" +
           std::to_string(file_counter_.fetch_add(1)) + extension;
}

int64_t PlaybackManager::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string PlaybackManager::escape_shell(const std::string& input) {
    std::string output = "'";
    for (const char c : input) {
        if (c == '\'')
            output += "'\\''";
        else
            output += c;
    }
    output += "'";
    return output;
}

std::string PlaybackManager::sanitize_tts_text(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    bool last_space = false;

    for (const unsigned char c : input) {
        if (c < 0x80) {
            if (std::isspace(c)) {
                if (!last_space) output.push_back(' ');
                last_space = true;
                continue;
            }
            last_space = false;
            switch (c) {
                case '*': case '#': case '`': case '~': case '_':
                case '[': case ']': case '{': case '}': case '<': case '>':
                case '|': case '/': case '\\': case '@': case '^':
                    continue;
                case ',': output += "，"; continue;
                case '.': output += "。"; continue;
                case '!': output += "！"; continue;
                case '?': output += "？"; continue;
                case ';': output += "；"; continue;
                case ':': output += "："; continue;
                case '"': case '\'': case '(': case ')': case '-':
                    continue;
                default:
                    output.push_back(static_cast<char>(c));
                    continue;
            }
        }
        output.push_back(static_cast<char>(c));
        last_space = false;
    }

    while (!output.empty() &&
           std::isspace(static_cast<unsigned char>(output.back())))
        output.pop_back();
    return output;
}
