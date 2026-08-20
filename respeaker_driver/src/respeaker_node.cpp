#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/int16_multi_array.hpp>

#include "audio_capture.hpp"
#include "respeaker_usb.hpp"

#include <thread>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cmath>
#include <stdexcept>

class ReSpeakerNode : public rclcpp::Node {
public:
    ReSpeakerNode()
        : Node("respeaker_node"),
          audio_("ReSpeaker", 1, 16000, 512)
    {
        audio_pub_ = create_publisher<std_msgs::msg::Int16MultiArray>("audio_raw", 10);
        vad_pub_   = create_publisher<std_msgs::msg::Bool>("vad", 10);
        doa_pub_   = create_publisher<std_msgs::msg::Float32>("doa", 10);

        // ----- 软件 VAD 参数（可通过 ROS2 parameter 覆盖） -----
        use_hw_vad_  = declare_parameter("use_hardware_vad", true);
        snr_threshold_ = declare_parameter("snr_threshold", 3.0);
        hold_off_ms_   = declare_parameter("hold_off_ms", 200);
        hold_off_frames_ = std::max(1, hold_off_ms_ / 20);  // timer 是 20ms 周期

        // ----- 硬件 USB：只读 VAD，不 claim 接口 -----
        if (use_hw_vad_ && usb_.open()) {
            RCLCPP_INFO(get_logger(), "硬件 VAD 已连接 (XVF-3000)");
            usb_ok_ = true;
            usb_running_ = true;
            usb_thread_  = std::thread(&ReSpeakerNode::usb_loop, this);
        } else {
            RCLCPP_WARN(get_logger(), "USB 不可用，使用软件 VAD fallback");
        }

        // ----- 音频采集 -----
        if (!audio_.start()) {
            usb_running_ = false;
            if (usb_thread_.joinable()) usb_thread_.join();
            usb_.close();
            throw std::runtime_error("无法启动 ReSpeaker ALSA 音频采集");
        }
        RCLCPP_INFO(get_logger(), "音频采集已启动 (16000 Hz)");
        audio_running_ = true;
        audio_thread_  = std::thread(&ReSpeakerNode::audio_loop, this);

        // ----- 定时器：50Hz -----
        timer_ = create_wall_timer(std::chrono::milliseconds(20),
                                   std::bind(&ReSpeakerNode::on_timer, this));
    }

    ~ReSpeakerNode() {
        usb_running_ = false;
        if (usb_thread_.joinable()) usb_thread_.join();
        audio_running_ = false;
        if (audio_thread_.joinable()) audio_thread_.join();
        usb_.close();
        audio_.stop();
    }

private:
    // publishers
    rclcpp::Publisher<std_msgs::msg::Int16MultiArray>::SharedPtr audio_pub_;
    rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr             vad_pub_;
    rclcpp::Publisher<std_msgs::msg::Float32>::SharedPtr          doa_pub_;
    rclcpp::TimerBase::SharedPtr                                   timer_;

    // 驱动
    AudioCapture  audio_;
    ReSpeakerUSB  usb_;

    // ====== USB 线程（硬件 VAD） ======
    std::thread       usb_thread_;
    std::atomic<bool> usb_running_{false};
    std::atomic<bool> usb_ok_{false};
    std::atomic<int>  hw_vad_{0};       // 硬件 VAD 最新值

    void usb_loop() {
        int consecutive_failures = 0;
        while (usb_running_ && rclcpp::ok()) {
            // 读硬件 VAD
            int v = usb_.read_vad();
            if (v == 0 || v == 1) {
                hw_vad_ = v;
                consecutive_failures = 0;
                usb_ok_ = true;
            } else if (++consecutive_failures >= 3) {
                if (usb_ok_.exchange(false)) {
                    RCLCPP_WARN(get_logger(),
                                "USB VAD 连续读取失败，切换到软件 VAD");
                }
            }

            // 读 DoA 角度并发布
            int a = usb_.read_doa();
            if (a >= 0 && a <= 359) {
                auto m = std_msgs::msg::Float32();
                m.data = static_cast<float>(a);
                doa_pub_->publish(m);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // ====== 音频线程 ======
    std::thread       audio_thread_;
    std::atomic<bool> audio_running_{false};

    std::deque<int16_t> ring_buf_;
    std::mutex          buf_mutex_;
    static constexpr size_t MAX_BUF = 16000;

    void audio_loop() {
        while (audio_running_ && rclcpp::ok()) {
            std::vector<int16_t> buf;
            int n = audio_.read(buf);
            if (n > 0) {
                auto msg = std_msgs::msg::Int16MultiArray();
                msg.data.insert(msg.data.end(), buf.begin(), buf.begin() + n);
                audio_pub_->publish(msg);

                std::lock_guard<std::mutex> lock(buf_mutex_);
                ring_buf_.insert(ring_buf_.end(), buf.begin(), buf.begin() + n);
                while (ring_buf_.size() > MAX_BUF)
                    ring_buf_.pop_front();
            }
        }
    }

    // ====== 软件 VAD（自适应噪声门） ======
    double   noise_floor_   = 50.0;
    double   signal_level_  = 50.0;
    bool     sw_voice_      = false;
    int      hold_on_       = 0;
    int      hold_off_      = 0;
    uint64_t frame_count_   = 0;

    static constexpr double  ALPHA_NOISE    = 0.02;
    static constexpr double  ALPHA_SIGNAL   = 0.15;
    static constexpr double  MIN_NOISE      = 30.0;
    static constexpr int     HOLD_ON_FRAMES  = 1;   // 20ms

    // 运行时参数（可通过 ROS2 parameter 覆盖）
    double snr_threshold_ = 3.0;
    int    hold_off_ms_   = 320;
    int    hold_off_frames_ = 16;  // hold_off_ms / 20
    bool   use_hw_vad_   = true;

    // ====== 定时器：决定最终 VAD ======
    void on_timer() {
        frame_count_++;

        // 1. 软件 VAD 计算
        std::vector<int16_t> recent;
        {
            std::lock_guard<std::mutex> lock(buf_mutex_);
            size_t n = std::min(ring_buf_.size(), size_t(400));
            if (n >= 400) {
                recent.assign(ring_buf_.end() - n, ring_buf_.end());
            }
        }

        if (!recent.empty()) {
            double sum = 0;
            for (auto s : recent) sum += (double)s * s;
            double rms = std::sqrt(sum / recent.size());
            signal_level_ = ALPHA_SIGNAL * rms + (1.0 - ALPHA_SIGNAL) * signal_level_;

            if (signal_level_ < noise_floor_ * 1.5) {
                noise_floor_ = ALPHA_NOISE * signal_level_
                             + (1.0 - ALPHA_NOISE) * noise_floor_;
                if (noise_floor_ < MIN_NOISE) noise_floor_ = MIN_NOISE;
            }

            double snr = signal_level_ / std::max(noise_floor_, 1.0);
            if (snr > snr_threshold_) { hold_on_++;  hold_off_ = 0; }
            else                      { hold_off_++; hold_on_  = 0; }
            if (hold_on_  >= HOLD_ON_FRAMES)  sw_voice_ = true;
            if (hold_off_ >= hold_off_frames_) sw_voice_ = false;
        }

        // 2. 决定最终 VAD
        bool vad;
        if (use_hw_vad_ && usb_ok_) {
            vad = (hw_vad_ == 1);
        } else {
            vad = sw_voice_;
        }

        // 3. 发布
        auto m = std_msgs::msg::Bool();
        m.data = vad;
        vad_pub_->publish(m);

        // 4. 每秒诊断
        if (frame_count_ % 50 == 0) {
            double snr = signal_level_ / std::max(noise_floor_, 1.0);
            RCLCPP_INFO(get_logger(),
                "VAD=%d | rms=%.0f noise=%.0f snr=%.1f thr=%.1f off=%dms (%s)",
                vad ? 1 : 0,
                signal_level_, noise_floor_, snr,
                snr_threshold_, hold_off_ms_,
                (use_hw_vad_ && usb_ok_) ? "hardware" : "software");
        }
    }
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    try {
        rclcpp::spin(std::make_shared<ReSpeakerNode>());
    } catch (const std::exception& e) {
        RCLCPP_FATAL(rclcpp::get_logger("respeaker_node"), "%s", e.what());
        rclcpp::shutdown();
        return 1;
    }
    rclcpp::shutdown();
    return 0;
}
