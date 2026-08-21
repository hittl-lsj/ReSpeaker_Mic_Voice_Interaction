# voice_interaction

`voice_interaction` 是机器人的语音交互核心包，负责：

- 唤醒词检测
- 音频预录和单句切分
- Sherpa-ONNX 本地 ASR
- LLM 对话和上下文历史
- 流式回答、断句和 TTS 播放
- 连续对话、告别、超时和插话

## 运行入口

启动文件：

```bash
ros2 launch voice_interaction voice_assistant.launch.py
```

默认参数文件：

```text
config/voice_assistant.yaml
```

启动文件会同时启动 `respeaker_driver/respeaker_node` 和本包的
`voice_interaction_node`。也可以传入独立配置：

```bash
ros2 launch voice_interaction voice_assistant.launch.py \
  config_file:=/path/to/voice_assistant.yaml
```

默认配置里使用了 `$(env HOME)` 路径替换。该替换由 launch 文件处理；
直接 `ros2 run ... --params-file` 不会展开。

启动前可先在仓库根目录运行环境体检：

```bash
./scripts/check_voice_env.sh
```

脚本只做只读检查，不启动 ROS 节点，也不会占用麦克风。

## 内部架构

```text
ROS topics
    |
    v
InteractionNode
    |
    v
    SessionStateMachine
    |-- SpeechRecorder
    |     |-- VAD 状态
    |     |-- 轻量 KWS / ASR 唤醒 fallback
    |     `-- 指令 ASR
    |
    |-- ResponseStreamer
    |     |-- LLM 流式响应
    |     |-- 句子队列
    |     `-- 合成线程 / 播放线程
    |
    `-- PlaybackManager
          |-- edge-tts
          |-- Piper
          |-- espeak-ng
          `-- ffplay 进程
```

### 组件职责

| 组件 | 职责 |
| --- | --- |
| `InteractionNode` | ROS 参数、订阅、定时器和会话对象的适配层 |
| `SessionStateMachine` | `IDLE`、`LISTENING`、`THINKING`、`SPEAKING` 状态及代际取消 |
| `SpeechRecorder` | 预录缓冲、VAD 结束、唤醒词匹配、ASR 转录 |
| `ResponseStreamer` | LLM 流式 token、断句、TTS 合成和播放队列 |
| `PlaybackManager` | TTS fallback、音频文件、播放进程和停止操作 |
| `LLMClient` | OpenAI-compatible / Anthropic provider 和 HTTP 请求 |
| `ASRSherpa` | Sherpa-ONNX streaming Zipformer C API 封装 |
| `KeywordSpotter` | Sherpa-ONNX 轻量 KWS C API 封装 |

## 会话行为

### 只有唤醒词

例如：

```text
小萝卜头
```

系统会播放：

```text
你好，有什么可以帮你吗？
```

### 唤醒词和命令同句

例如：

```text
小萝卜头，打开客厅灯
```

唤醒词命中后不会先播放提示音。系统会继续录制当前句，并把唤醒词和后续命令交给最终 ASR，再从文本开头移除唤醒词。

`wake_preroll_ms` 决定唤醒触发时从预录缓冲中保留多长音频。唤醒词较长或用户说话较快时，应确保该值覆盖完整唤醒词。

### 唤醒检测模式

通过 `wake_detector` 选择唤醒检测方式：

- `kws`：使用轻量 Sherpa-ONNX KWS，默认模式。
- `asr`：使用完整 ASR 的 partial result，兼容旧模式。
- `off`：关闭唤醒词，直接由 VAD 触发。

`kws` 模式需要单独的 KWS 模型。模型目录默认是：

```text
models/sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01-mobile/
```

默认 Wenetspeech KWS 模型使用拼音建模，关键词文件要按声母和韵母拆分：

```text
x iǎo l uó b o t óu @小萝卜头
```

如果 KWS 模型、模型文件或关键词文件不可用，程序会自动回退到 `asr` 模式。
因此可以先部署代码，再准备 KWS 模型。

### 连续对话

第一次唤醒后进入连续对话。之后检测到 VAD 语音即可开始录音，不需要重复唤醒。超过 `conversation_timeout_sec` 没有成功识别，会回到待唤醒状态。

### 插话

机器人处于播报状态时，如果 VAD 连续保持超过 `barge_in_hold_ms`，系统会：

1. 取消当前 LLM 请求
2. 停止当前音频播放
3. 保留插话确认窗口的预录音频
4. 重新进入录音状态

插话是否可靠取决于麦克风阵列的 AEC/VAD。机器人声音被自己打断时，优先增大 `barge_in_guard_ms`，或者暂时关闭 `enable_barge_in`。

## 关键参数

| 参数 | 作用 |
| --- | --- |
| `asr_model_dir` | Sherpa-ONNX 模型目录 |
| `wake_word` | 主唤醒词 |
| `wake_word_aliases` | 唤醒词同音、繁体或常见 ASR 输出 |
| `wake_detector` | `kws`、`asr` 或 `off` |
| `kws_model_dir` | KWS 模型目录 |
| `kws_keywords_file` | KWS 关键词文件 |
| `kws_keywords_score` | KWS 关键词加分 |
| `kws_keywords_threshold` | KWS 触发阈值 |
| `preroll_ms` | 普通 VAD 触发时保留的预录时长 |
| `wake_preroll_ms` | 唤醒触发时保留的预录时长 |
| `silence_sec` | 结束一句话所需的静音时间 |
| `max_utterance_sec` | 单句最大录音时间 |
| `conversation_timeout_sec` | 连续对话超时时间 |
| `enable_barge_in` | 是否允许插话 |
| `barge_in_hold_ms` | 确认插话所需的 VAD 持续时间 |
| `barge_in_guard_ms` | 每段播报开始后的插话保护时间 |
| `tts_device` | `ffplay` 音频输出设备；空或 `default` 使用系统默认输出 |
| `piper_model` | Piper 中文模型路径 |

## Provider 顺序

如果配置完整，网关 provider 优先于 Ollama：

```text
OpenAI-compatible gateway -> Ollama -> 无可用 provider
```

网关密钥只从环境变量读取：

```bash
export VOICE_GATEWAY_API_KEY='your-api-key'
```

Ollama 默认配置：

```yaml
use_ollama: true
ollama_url: "http://localhost:11434/v1"
ollama_model: "qwen2.5:1.5b"
```

## TTS fallback

普通播报和句子流水线都会按照下面顺序尝试：

```text
edge-tts -> Piper -> espeak-ng
```

`edge-tts` 连续失败后会暂时冷却，避免网络故障时每一句都阻塞等待。Piper 和 espeak-ng 用于离线兜底。

## 调试

检查节点和话题：

```bash
ros2 node list
ros2 topic list
ros2 topic echo /vad
ros2 topic echo /audio_raw
```

检查参数：

```bash
ros2 param list /voice_interaction_node
ros2 param get /voice_interaction_node wake_word
ros2 param get /voice_interaction_node wake_preroll_ms
```

如果完全没有识别结果，按以下顺序排查：

1. 先运行 `./scripts/check_voice_env.sh`。
2. 确认 `respeaker_node` 正常运行。
3. 确认 `/audio_raw` 有持续消息。
4. 确认 `/vad` 在说话时变为 `true`。
5. 确认 Sherpa 模型目录和文件名正确。
6. 确认 KWS 模型目录和关键词文件正确；缺失时查看日志中的 ASR fallback。
7. 确认 Ollama 或网关可以访问。
8. 确认 `ffplay`、Piper 模型和 `tts_device` 可用。

## 设计约束

- 默认使用独立的 KWS recognizer 和指令 ASR；KWS 不可用时才额外创建 ASR 唤醒 fallback。
- 音频回调只负责收集音频和喂唤醒 recognizer，ASR、LLM、TTS 在 worker 或专用线程中执行。
- 每轮会话拥有一个 generation；插话、超时和关闭都会使旧 generation 失效。
- TTS 合成和播放使用唯一临时文件，避免多句流水线互相覆盖。
- `voice_interfaces/srv/SetLED.srv` 当前只保留接口定义，驱动尚未注册 `set_led` 服务。
