#include "respeaker_usb.hpp"
#include <iostream>
#include <cstring>

ReSpeakerUSB::ReSpeakerUSB() : handle_(nullptr) {}

ReSpeakerUSB::~ReSpeakerUSB() {
    close();
}

bool ReSpeakerUSB::open() {
    // 1. 初始化 libusb
    if (libusb_init(nullptr) < 0) {
        std::cerr << "[ReSpeakerUSB] libusb_init 失败" << std::endl;
        return false;
    }

    // 2. 按 VID/PID 找到并打开 ReSpeaker
    handle_ = libusb_open_device_with_vid_pid(nullptr, RESPEAKER_VID, RESPEAKER_PID);
    if (!handle_) {
        std::cerr << "[ReSpeakerUSB] 未找到设备 " << std::hex
                  << RESPEAKER_VID << ":" << RESPEAKER_PID << std::dec << std::endl;
        libusb_exit(nullptr);
        return false;
    }

    // 注意：不 detach/claim 接口！
    // Control transfer（DoA/VAD/LED）走 endpoint 0，不需要声明接口。
    // 内核的 snd-usb-audio 驱动继续负责音频流，互不干扰。

    std::cout << "[ReSpeakerUSB] 设备已打开 (无需 claim 接口)" << std::endl;
    return true;
}

void ReSpeakerUSB::close() {
    if (!handle_) return;
    libusb_close(handle_);
    libusb_exit(nullptr);
    handle_ = nullptr;
}

bool ReSpeakerUSB::is_open() const {
    return handle_ != nullptr;
}

// ========== DoA / VAD ==========

int ReSpeakerUSB::read_doa() {
    uint8_t data[8];
    // Python: bRequest=0, wValue=0xC0, wIndex=21
    int n = ctrl_in(DOA_WVALUE, PARAM_DOA_ID, data, 8);
    if (n != 8) return -1;
    int value = -1;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

int ReSpeakerUSB::read_vad() {
    uint8_t data[8];
    // Python: bRequest=0, wValue=0xE0, wIndex=19
    int n = ctrl_in(VAD_WVALUE, PARAM_VAD_ID, data, 8);
    if (n != 8) return -1;
    int value = -1;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

// ========== LED ==========

void ReSpeakerUSB::led_set_all(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t buf[36];
    for (int i = 0; i < 12; i++) {
        buf[i * 3]     = r;
        buf[i * 3 + 1] = g;
        buf[i * 3 + 2] = b;
    }
    led_send(LED_CMD_SET_ALL, buf, 36);
}

void ReSpeakerUSB::led_mono(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t buf[3] = {r, g, b};
    led_send(LED_CMD_MONO, buf, 3);
}

void ReSpeakerUSB::led_set_brightness(uint8_t level) {
    uint8_t buf[1] = {level};
    led_send(LED_CMD_BRIGHTNESS, buf, 1);
}

void ReSpeakerUSB::led_trace() {
    led_send(LED_CMD_TRACE, nullptr, 0);
}

void ReSpeakerUSB::led_spin(uint8_t speed) {
    uint8_t buf[1] = {speed};
    led_send(LED_CMD_SPIN, buf, 1);
}

void ReSpeakerUSB::led_breathe(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t buf[3] = {r, g, b};
    led_send(LED_CMD_BREATHE, buf, 3);
}

// ========== 底层 USB 传输 ==========

int ReSpeakerUSB::ctrl_in(uint16_t wValue, uint16_t wIndex, uint8_t* data, int length) {
    if (!handle_) return -1;
    // 对应 tuning.py: bmRequestType=0xC0, bRequest=0, wValue=cmd, wIndex=id
    return libusb_control_transfer(handle_,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN,
        0,         // bRequest = 0（与 Python 一致）
        wValue,    // 命令字节
        wIndex,    // 参数 ID
        data, length, USB_TIMEOUT_MS);
}

void ReSpeakerUSB::led_send(uint8_t cmd, uint8_t* data, int len) {
    if (!handle_) return;
    libusb_control_transfer(handle_,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
        0, cmd, LED_WVALUE, data, len, USB_TIMEOUT_MS);
}
