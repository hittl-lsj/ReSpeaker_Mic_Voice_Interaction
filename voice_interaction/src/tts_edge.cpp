#include "tts_edge.hpp"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>

TTSEdge::TTSEdge(const std::string& voice, const std::string& audio_dev)
    : voice_(voice), audio_dev_(audio_dev) {
    tmp_txt_ = "/tmp/va_text.txt";
    tmp_mp3_ = "/tmp/va_tts.mp3";
}

TTSEdge::~TTSEdge() {
    stop();
    std::remove(tmp_txt_.c_str());
    std::remove(tmp_mp3_.c_str());
}

void TTSEdge::kill_ffplay() {
    int pid = ffplay_pid_.exchange(0);
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
    }
}

void TTSEdge::stop() {
    kill_ffplay();
    playing_ = false;
}

bool TTSEdge::speak(const std::string& text) {
    if (text.empty()) return false;

    // 写文本文件
    std::ofstream ofs(tmp_txt_);
    if (!ofs) { std::cerr << "[TTS] 写文件失败" << std::endl; return false; }
    ofs << text;
    ofs.close();

    // 合成
    std::string cmd = "edge-tts --voice " + voice_
                    + " --file " + tmp_txt_
                    + " --write-media " + tmp_mp3_
                    + " 2>/dev/null";
    if (system(cmd.c_str()) != 0) {
        std::cerr << "[TTS] 合成失败" << std::endl;
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (!audio_dev_.empty()) setenv("AUDIODEV", audio_dev_.c_str(), 1);
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
               "-loglevel", "quiet", "-af", "channelmap=0-0|0-1",
               tmp_mp3_.c_str(), nullptr);
        _exit(1);
    }
    ffplay_pid_.store(pid);
    playing_ = true;
    wait_done();
    return true;
}

bool TTSEdge::speak_async(const std::string& text) {
    if (text.empty()) return false;

    // 写文本
    std::ofstream ofs(tmp_txt_);
    if (!ofs) return false;
    ofs << text;
    ofs.close();

    // 合成
    std::string cmd = "edge-tts --voice " + voice_
                    + " --file " + tmp_txt_
                    + " --write-media " + tmp_mp3_
                    + " 2>/dev/null";
    if (system(cmd.c_str()) != 0) return false;

    // 后台播放
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // 子进程：指定输出设备 + 播放
        if (!audio_dev_.empty())
            setenv("AUDIODEV", audio_dev_.c_str(), 1);
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
               "-loglevel", "quiet", "-af", "channelmap=0-0|0-1",
               tmp_mp3_.c_str(), nullptr);
        _exit(1);
    }
    ffplay_pid_.store(pid);
    playing_ = true;
    return true;
}

bool TTSEdge::synthesize(const std::string& text, const std::string& mp3_path) {
    if (text.empty()) return false;

    std::ofstream ofs(tmp_txt_);
    if (!ofs) return false;
    ofs << text;
    ofs.close();

    std::string cmd = "edge-tts --voice " + voice_
                    + " --file " + tmp_txt_
                    + " --write-media " + mp3_path
                    + " 2>/dev/null";
    if (system(cmd.c_str()) != 0) return false;
    return true;
}

void TTSEdge::wait_done() {
    int pid = ffplay_pid_.exchange(0);
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
    playing_ = false;
}

bool TTSEdge::is_playing() {
    int pid = ffplay_pid_.load();
    if (pid <= 0) return false;
    int status = 0;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result != 0) {
        int expected = pid;
        ffplay_pid_.compare_exchange_strong(expected, 0);
        playing_ = false;
        return false;
    }
    return result == 0;
}
