import os
from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    home = os.path.expanduser("~")
    return LaunchDescription([

        # ====== respeaker_node：音频采集 + VAD + DoA + LED ======
        Node(
            package='respeaker_driver',
            executable='respeaker_node',
            name='respeaker_node',
            parameters=[{
                # VAD 模式：true=硬件VAD(XVF-3000), false=软件VAD
                'use_hardware_vad': True,

                # 软件 VAD 参数（仅 use_hardware_vad=False 时生效）
                'snr_threshold': 3.0,      # 信噪比阈值，越低越灵敏
                'hold_off_ms': 200,         # VAD=1→0 后等待多少 ms 才确认"说完了"

                # LED
                'led_enabled': True,
                'led_brightness': 10,
            }],
        ),

        # ====== interaction_node：ASR → LLM → TTS ======
        Node(
            package='voice_interaction',
            executable='interaction_node',
            name='voice_interaction_node',
            parameters=[{
                # ----- 语音识别 -----
                'asr_model_dir': home + '/ReSpearMicArray/models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20',
                'silence_sec': 1.0,        # VAD 静音后等待几秒判"说完了"
                'preroll_ms': 500,          # VAD 触发前补多少 ms 音频（防开头截断）

                # ----- 唤醒词 -----
                'use_wake_word': True,      # True=需唤醒词触发；False=纯 VAD 触发
                'wake_word': '你好',        # 唤醒词（Vosk 本地识别，说"你好"+指令）
                'conversation_timeout_sec': 20.0,  # 唤醒后连续对话窗口，无识别则超时重新锁定

                # ----- LLM -----
                'system_prompt': '你好你是一个语音助手。和你对话的是一个小朋友。请用简短的中文回答他的问题。不超过100字。',
                'max_history': 6,           # 保留几轮对话

                # HTTP 代理（开发机走本地代理；X5 部署时直连留空）
                'http_proxy': '',

                # Ollama 本地（离线 fallback；X5 上跑 qwen 小模型）
                'use_ollama': True,
                'ollama_url': 'http://localhost:11434/v1',
                'ollama_model': 'qwen2.5:1.5b',

                # ----- TTS -----
                'tts_voice': 'zh-CN-XiaoxiaoNeural',   # edge-tts 发音人
                'tts_device': 'plughw:3,0',            # 输出设备（plughw:3,0 = 开发机上的 USB2.0 Device；不同机器卡号不同，用 aplay -l 查）

                # piper 离线 fallback（edge-tts 失败时自动切换）
                'piper_model': home + '/ReSpearMicArray/zh_CN-huayan-medium.onnx',
            }],
        ),

    ])
