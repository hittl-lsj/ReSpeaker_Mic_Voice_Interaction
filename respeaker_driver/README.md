# respeaker_driver

`respeaker_driver` 是 ReSpeaker USB 4 Mic Array 的 ROS 2 驱动包，负责：

- ALSA 音频采集
- ReSpeaker USB 硬件 VAD
- USB DoA 声源方向读取
- 软件 VAD fallback
- 发布语音交互包需要的 ROS topics

## 启动

通常由统一 launch 文件启动：

```bash
ros2 launch voice_interaction voice_assistant.launch.py
```

单独启动驱动：

```bash
ros2 run respeaker_driver respeaker_node
```

## ROS 接口

| Topic | 类型 | 说明 |
| --- | --- | --- |
| `/audio_raw` | `std_msgs/msg/Int16MultiArray` | 16 kHz、16-bit、mono PCM |
| `/vad` | `std_msgs/msg/Bool` | 当前是否检测到语音 |
| `/doa` | `std_msgs/msg/Float32` | 声源方向，单位为度，范围 0 到 359 |

音频默认配置为：

```text
采样率: 16000 Hz
格式: S16_LE
声道: mono
周期: 512 frames
```

## 设备发现

采集设备由 `capture_device` 参数指定：

```yaml
respeaker_node:
  ros__parameters:
    capture_device: "plughw:2,0"
```

`capture_device` 可以填写任意 ALSA PCM 设备名，例如 `plughw:2,0`、`hw:1,0`
或 `default`。留空或设为 `auto` 时，驱动会遍历 ALSA 声卡，优先查找声卡名称中
包含 `ReSpeaker` 的设备；如果没有找到，会回退到旧默认值：

```text
plughw:3,0
```

查看实际设备：

```bash
aplay -l
arecord -l
```

如果使用普通 USB 麦克风，通常还应关闭硬件 VAD，让软件 VAD 接管：

```yaml
respeaker_node:
  ros__parameters:
    capture_device: "plughw:2,0"
    use_hardware_vad: false
```

## VAD 模式

默认优先使用 XVF-3000 硬件 VAD：

```yaml
respeaker_node:
  ros__parameters:
    use_hardware_vad: true
```

硬件 VAD 通过 USB 控制传输读取。如果设备不存在、USB 连续读取失败，驱动会自动切换到软件 VAD。

软件 VAD 使用最近的音频计算 RMS，并维护自适应噪声底：

```yaml
respeaker_node:
  ros__parameters:
    snr_threshold: 3.0
    hold_off_ms: 200
```

参数含义：

- `snr_threshold`：信号相对于噪声底的比例阈值，增大后更不容易被噪声触发。
- `hold_off_ms`：从有语音切回静音前需要持续的静音时间，增大后更不容易截断句尾。
- `use_hardware_vad`：设为 `false` 可强制使用软件 VAD。

## USB 功能

当前 USB 层支持：

- 读取 DoA
- 读取 VAD
- 设置 LED 全色
- LED 单色模式
- 设置亮度
- 追踪、旋转和呼吸灯命令

驱动当前只注册音频、VAD 和 DoA topic，尚未注册 `set_led` ROS service。LED 底层方法保留在 `ReSpeakerUSB` 中，后续可以在节点层增加服务或状态映射。

## 常见排障

### 找不到声卡

```bash
arecord -l
cat /proc/asound/cards
```

确认设备已连接，并检查用户是否有音频设备权限：

```bash
groups
```

如果没有 `audio` 组权限：

```bash
sudo usermod -aG audio "$USER"
```

重新登录后再测试。

### ALSA 打开失败

先停止占用设备的进程：

```bash
fuser -v /dev/snd/*
```

然后确认采集设备支持 16 kHz、mono：

```bash
arecord -D plughw:2,0 -f S16_LE -r 16000 -c 1 -d 5 /tmp/respeaker_test.wav
```

### `/audio_raw` 没有消息

依次检查：

```bash
ros2 node list
ros2 topic hz /audio_raw
ros2 topic echo /audio_raw --once
```

如果节点启动但没有音频，重点查看启动日志中的 ALSA 设备名和 `snd_pcm_open` 错误。

### `/vad` 一直为 `false`

先暂时切换软件 VAD：

```yaml
use_hardware_vad: false
```

然后提高环境音量说话并观察每秒 VAD 诊断日志。如果软件 VAD 有效，问题通常位于 USB 设备访问、VID/PID、固件或硬件 VAD 参数。

### VAD 抖动

可以逐步调整：

```yaml
snr_threshold: 3.0
hold_off_ms: 300
```

噪声误触发时提高 `snr_threshold`；句尾被截断时提高 `hold_off_ms`。参数应结合实际房间噪声和扬声器回声测试。

## 构建

依赖 ALSA 和 libusb：

```bash
sudo apt install libasound2-dev libusb-1.0-0-dev
source /opt/ros/jazzy/setup.bash
colcon build --packages-select respeaker_driver
source install/setup.bash
```
