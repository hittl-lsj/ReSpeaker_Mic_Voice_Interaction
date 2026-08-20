# ReSpeaker Mic Voice Interaction

基于 ROS 2 和 ReSpeaker USB 4 Mic Array 的中文语音交互程序。系统从麦克风阵列采集音频，完成 VAD、唤醒词检测、本地语音识别、LLM 对话和语音合成播放。

## 数据流

```text
ReSpeaker
  |-- ALSA: 16 kHz / 16-bit / mono PCM
  |-- USB: hardware VAD + DoA
  v
respeaker_node
  |-- /audio_raw  std_msgs/Int16MultiArray
  |-- /vad        std_msgs/Bool
  |-- /doa        std_msgs/Float32
  v
interaction_node
  |-- Sherpa-ONNX ASR
  |-- Ollama (OpenAI-compatible API)
  `-- edge-tts -> Piper -> espeak-ng
```

## 目录

- `respeaker_driver/`: ALSA 音频采集、USB VAD/DoA 和软件 VAD 回退
- `voice_interaction/`: 唤醒、ASR、对话状态机、LLM 和 TTS
- `voice_interfaces/`: ROS 2 自定义服务接口
- `launch/`: 双节点启动文件
- `config/`: 参数配置示例

模型、预编译运行库、构建产物和设备厂商工具未纳入仓库。

## 环境要求

- Ubuntu 24.04、ROS 2 Jazzy、C++17、colcon
- ALSA 与 libusb 1.0 开发包
- libcurl 开发包
- Sherpa-ONNX 共享库发行包
- Ollama，默认模型为 `qwen2.5:1.5b`
- `edge-tts`、Piper、`ffplay` 和 `espeak-ng`

安装系统依赖：

```bash
sudo apt install libasound2-dev libusb-1.0-0-dev libcurl4-openssl-dev \
  ffmpeg espeak-ng python3-colcon-common-extensions
python3 -m pip install --user edge-tts piper-tts
ollama pull qwen2.5:1.5b
```

## 模型准备

下载 Sherpa-ONNX 的 streaming Zipformer 中英双语模型，并放到：

```text
models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20/
  encoder-epoch-99-avg-1.onnx
  decoder-epoch-99-avg-1.onnx
  joiner-epoch-99-avg-1.onnx
  tokens.txt
```

下载 Piper 中文模型，将文件放在工程根目录：

```text
zh_CN-huayan-medium.onnx
zh_CN-huayan-medium.onnx.json
```

另外下载与目标架构匹配的 Sherpa-ONNX shared library 包，并把 `SHERPA_ONNX_ROOT` 指向其解压目录。该目录应包含 `include/` 和 `lib/`。

## 构建

```bash
source /opt/ros/jazzy/setup.bash
export SHERPA_ONNX_ROOT=/path/to/sherpa-onnx-shared
colcon build --symlink-install
source install/setup.bash
```

## 启动

先启动 Ollama：

```bash
ollama serve
```

在另一个终端启动语音系统：

```bash
source /opt/ros/jazzy/setup.bash
source install/setup.bash
ros2 launch ./launch/voice_assistant.launch.py
```

默认唤醒词为“你好”。唤醒后进入连续对话模式，20 秒无有效识别会重新进入待唤醒状态。

使用 `aplay -l` 和 `arecord -l` 检查声卡编号，并按设备实际情况修改 `tts_device`。音频采集会优先按名称查找 ReSpeaker，找不到时回退到 `plughw:3,0`。

## 注意事项

- 不要把 API Key 写入 launch、YAML 或源码并提交到 Git。
- 硬件 VAD 或 USB 控制不可用时，驱动会自动使用软件 VAD。
- `SetLED` 接口和 USB LED 底层方法已经保留，但当前驱动尚未注册 `set_led` 服务。
- `config/voice_assistant.yaml` 是配置示例，当前启动文件使用内联参数。

## License

MIT
