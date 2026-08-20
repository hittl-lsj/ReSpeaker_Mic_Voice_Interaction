#pragma once

extern "C" {
#include <libusb-1.0/libusb.h>
}

#include <cstdint>

// ReSpeaker Mic Array VID/PID
#define RESPEAKER_VID  0x2886
#define RESPEAKER_PID  0x0018

// ==== 硬件参数 ID（来自 tuning.py）====
#define PARAM_DOA_ID      21   // DOA 角度参数
#define PARAM_DOA_OFFSET  0
#define PARAM_VAD_ID      19   // VAD / 语音活动检测参数
#define PARAM_VAD_OFFSET  32

// ==== 预计算的 wValue ====
// 计算公式（来自 tuning.py）: cmd = 0x80(读) | offset | 0x40(int类型)
#define DOA_WVALUE (0x80 | PARAM_DOA_OFFSET | 0x40)  // 0xC0
#define VAD_WVALUE (0x80 | PARAM_VAD_OFFSET | 0x40)  // 0xE0

// ==== LED 命令（来自 pixel_ring）====
#define LED_CMD_SET_ALL       0x00   // 12 颗 LED 统一设色
#define LED_CMD_MONO          0x01   // 单色模式
#define LED_CMD_BRIGHTNESS    0x02   // 亮度
#define LED_CMD_TRACE         0x08   // 跟随声源方向
#define LED_CMD_SPIN          0x09   // 旋转动画
#define LED_CMD_BREATHE       0x0A   // 呼吸灯
#define LED_WVALUE            0x1C   // LED 通信的固定 wValue

#define USB_TIMEOUT 100000  // USB 超时（微秒）


/**
 * @brief ReSpeaker Mic Array 的底层 USB 驱动
 *
 * 封装 libusb 控制传输，提供 DoA/VAD 读取和 LED 控制。
 * 不依赖 ROS2，可独立测试。
 */
class ReSpeakerUSB {
public:
    ReSpeakerUSB();
    ~ReSpeakerUSB();

    /** 打开 USB 设备（需先调用才能使用其他方法） */
    bool open();

    /** 关闭、释放资源 */
    void close();

    /** 设备是否已打开 */
    bool is_open() const;

    // ---------- DoA / VAD ----------

    /** 读取声源方向角度，返回 0~359，失败返回 -1 */
    int read_doa();

    /** 读取语音活动检测，返回 0（静音）或 1（有语音），失败返回 -1 */
    int read_vad();

    // ---------- LED ----------

    /** 设置全部 12 颗 LED 为同一颜色，每通道 0~255 */
    void led_set_all(uint8_t r, uint8_t g, uint8_t b);

    /** 单色模式：全部 LED 显示一种颜色 */
    void led_mono(uint8_t r, uint8_t g, uint8_t b);

    /** 设置亮度，level 范围 0x00~0x1F */
    void led_set_brightness(uint8_t level);

    /** 追踪模式：LED 跟随声源方向（需配合 DoA 数据） */
    void led_trace();

    /** 旋转动画，speed 控制速度 */
    void led_spin(uint8_t speed);

    /** 呼吸灯效果 */
    void led_breathe(uint8_t r, uint8_t g, uint8_t b);

private:
    libusb_device_handle* handle_;

    /**
     * @brief 控制传输（读方向：设备 → 主机）
     * @param wValue 命令字节（0x80|offset|0x40）
     * @param wIndex 参数 ID
     * @param data   接收缓冲区
     * @param length 期望读取长度
     * @return 实际读取的字节数，失败返回 -1
     */
    int ctrl_in(uint16_t wValue, uint16_t wIndex, uint8_t* data, int length);

    /**
     * @brief 控制传输（写方向：主机 → 设备）
     * @param cmd   命令字节
     * @param data  发送数据
     * @param len   数据长度
     */
    void led_send(uint8_t cmd, uint8_t* data, int len);
};
