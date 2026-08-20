#include "tts_piper.hpp"
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>

TTSPiper::TTSPiper(const std::string& model_path, const std::string& audio_dev)
    : model_path_(model_path), audio_dev_(audio_dev) {
    tmp_txt_ = "/tmp/va_piper.txt";
    tmp_wav_ = "/tmp/va_piper.wav";
}

TTSPiper::~TTSPiper() {
    stop();
    std::remove(tmp_txt_.c_str());
    std::remove(tmp_wav_.c_str());
}

void TTSPiper::kill_ffplay() {
    if (ffplay_pid_ > 0) {
        kill(ffplay_pid_, SIGTERM);
        waitpid(ffplay_pid_, nullptr, WNOHANG);
        ffplay_pid_ = 0;
    }
}

void TTSPiper::stop() {
    kill_ffplay();
    playing_ = false;
}

bool TTSPiper::speak(const std::string& text) {
    if (text.empty()) return false;

    // 写入文本文件（避免 shell 转义问题）
    std::ofstream ofs(tmp_txt_);
    if (!ofs) { std::cerr << "[piper] 写文本失败" << std::endl; return false; }
    ofs << text;
    ofs.close();

    // piper 从文件读取，命令行参数安全
    std::string cmd = "piper -m " + model_path_ + " -i " + tmp_txt_ + " -f " + tmp_wav_;
    if (system(cmd.c_str()) != 0) {
        std::cerr << "[piper] 合成失败: " << cmd << std::endl;
        return false;
    }

    // 播放
    // channelmap=0-0|0-1：将输入声道0复制到两个输出声道
    // ReSpeaker 4 Mic Array 耳机孔硬件只有左声道，此滤镜确保内容不丢失
    std::string play;
    if (!audio_dev_.empty())
        play = "AUDIODEV=" + audio_dev_ + " ffplay -nodisp -autoexit -loglevel quiet"
               " -af \"channelmap=0-0|0-1\" " + tmp_wav_;
    else
        play = "ffplay -nodisp -autoexit -loglevel quiet"
               " -af \"channelmap=0-0|0-1\" " + tmp_wav_;
    system(play.c_str());
    return true;
}

bool TTSPiper::speak_async(const std::string& text) {
    if (text.empty()) return false;

    // 写入文本文件（避免 shell 转义问题）
    std::ofstream ofs(tmp_txt_);
    if (!ofs) { std::cerr << "[piper] 写文本失败" << std::endl; return false; }
    ofs << text;
    ofs.close();

    // piper 从文件读取，命令行参数安全
    std::string cmd = "piper -m " + model_path_ + " -i " + tmp_txt_ + " -f " + tmp_wav_;
    if (system(cmd.c_str()) != 0) {
        std::cerr << "[piper] 合成失败: " << cmd << std::endl;
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (!audio_dev_.empty())
            setenv("AUDIODEV", audio_dev_.c_str(), 1);
        // channelmap=0-0|0-1：确保两个声道都从输入声道0复制（硬件只有左声道）
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit",
               "-loglevel", "quiet", "-af", "channelmap=0-0|0-1",
               tmp_wav_.c_str(), nullptr);
        _exit(1);
    }
    ffplay_pid_ = pid;
    playing_ = true;
    return true;
}

bool TTSPiper::synthesize(const std::string& text, const std::string& wav_path) {
    if (text.empty()) return false;

    // 写文本文件（避免 shell 转义问题）
    std::ofstream ofs(tmp_txt_);
    if (!ofs) { std::cerr << "[piper] 写文本失败" << std::endl; return false; }
    ofs << text;
    ofs.close();

    // piper 合成到指定文件（不播放）
    std::string cmd = "piper -m " + model_path_ + " -i " + tmp_txt_ + " -f " + wav_path;
    if (system(cmd.c_str()) != 0) {
        std::cerr << "[piper] 合成失败: " << cmd << std::endl;
        return false;
    }
    return true;
}

void TTSPiper::wait_done() {
    if (ffplay_pid_ > 0) {
        int status;
        waitpid(ffplay_pid_, &status, 0);
        ffplay_pid_ = 0;
    }
    playing_ = false;
}
