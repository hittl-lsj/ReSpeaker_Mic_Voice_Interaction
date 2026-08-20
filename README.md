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
- `voice_interaction/`: 唤醒、ASR、对话状态机、LLM、TTS、启动和统一配置
- `voice_interfaces/`: ROS 2 自定义服务接口

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
ros2 launch voice_interaction voice_assistant.launch.py
```

所有默认部署参数统一保存在
`voice_interaction/config/voice_assistant.yaml`，包括 VAD、模型路径、唤醒词、
Ollama 地址和 TTS 声卡。其他机器可传入独立配置文件：

```bash
ros2 launch voice_interaction voice_assistant.launch.py \
  config_file:=/path/to/voice_assistant.yaml
```

使用 OpenAI 兼容网关时，在 YAML 中填写 `gateway_base_url` 和
`gateway_model`，密钥通过环境变量提供：

```bash
export VOICE_GATEWAY_API_KEY='your-api-key'
ros2 launch voice_interaction voice_assistant.launch.py
```

唤醒词由 `wake_word` 配置，`wake_word_aliases` 可填写 ASR 常见的同音或繁体输出。
当前示例使用“小萝卜头”，但仍要求识别到完整四字短语，不会因为只识别到“小萝卜”就提前触发。
唤醒词命中后不会立即播放提示音，而是继续录完整句，因此支持“小萝卜头，打开客厅灯”这类唤醒词和命令同句的说法。
唤醒后进入连续对话模式，20 秒无有效识别会重新进入待唤醒状态。

会话边界和插话参数位于统一 YAML：

- `wait_user_timeout_sec`: 唤醒后等待用户开口的最长时间
- `wake_preroll_ms`: 唤醒触发时保留唤醒词及其后紧邻命令的预录时长
- `max_utterance_sec`: 单句最长录音时间
- `tts_cooldown_ms`: TTS 播放结束后的 VAD/唤醒冷却时间
- `enable_barge_in`: 是否允许用户在机器人播报或思考时插话
- `barge_in_hold_ms`: VAD 连续保持多久才确认插话
- `barge_in_guard_ms`: 每段 TTS 开始后暂不检测插话的保护时间

Barge-in 依赖麦克风阵列的 AEC/VAD 效果。若扬声器回声导致机器人打断自己，优先增大
`barge_in_guard_ms` 和 `barge_in_hold_ms`，必要时暂时关闭 `enable_barge_in`。

使用 `aplay -l` 和 `arecord -l` 检查声卡编号，并按设备实际情况修改 `tts_device`。音频采集会优先按名称查找 ReSpeaker，找不到时回退到 `plughw:3,0`。

## 注意事项

- 不要把 API Key 写入 launch、YAML 或源码；网关密钥使用 `VOICE_GATEWAY_API_KEY`。
- 硬件 VAD 或 USB 控制不可用时，驱动会自动使用软件 VAD。
- `SetLED` 接口和 USB LED 底层方法已经保留，但当前驱动尚未注册 `set_led` 服务。
- 源码中的 `declare_parameter()` 只提供安全默认值，部署参数以 YAML 为准。

## License

MIT
