# Friday - 钢铁侠智能助手 · 全屋智能系统 · AI 统一入口

钢铁侠的 Friday，开源实现。全屋智能的 AI 大脑，所有设备的统一入口。

> **Friday = 眼睛 + 嘴巴**
> - **眼睛**: 全双工视频理解，实时描述画面
> - **嘴巴**: 语音播报、TTS 通知
> - 不决策、不调度，只负责看和说
>
> **my-agent**（大脑）统一调度，对接大模型 API，协调 Friday/Coding Agent/微信。
> **微信** 是通知渠道和消息入口。

基于 [MiniCPM-o 4.5](https://github.com/OpenBMB/MiniCPM-o) + [llama.cpp-omni](https://github.com/tc-mb/llama.cpp-omni) 构建。

> **愿景**: 像钢铁侠的 Friday 一样——摄像头看着、AI 想着、设备动着。一个 AI 入口，管全家。

感谢 [OpenBMB/MiniCPM-o-Demo](https://github.com/OpenBMB/MiniCPM-o-Demo) 开源的模型及 Demo。

[设计文档 →](chat/design.md)

## 项目历程

从 Web 界面 → C++ 二进制 → 单文件部署，跑在 Ubuntu 系统上。

- 原版官方 Demo 基于 Python Web 服务
- 重构为纯 C++ 二进制（gui2），直接链接 libomni.so
- 支持 Lua 脚本热更新（custom_server/server.lua）
- 最终形态：一个 190KB 二进制 + 播放脚本，零 Python 依赖

## 项目目的

构建钢铁侠 Friday 式的全屋智能 AI 系统：

| 能力 | 说明 |
|------|------|
| 👁️ **摄像头看见** | 全双工视频监控，AI 实时理解画面 |
| 🧠 **AI 思考决策** | 何时说话、何时报警、何时联动设备 |
| ⚡ **设备自动执行** | Lua 脚本热更新，对接 MQTT/HTTP/GPIO |
| 🎙️ **统一入口** | 语音交互，一个 AI 管全家 |

从 Web 界面 → C++ 二进制 → 单文件部署。

## 技术栈

| 组件 | 技术 | 说明 |
|------|------|------|
| 模型推理 | MiniCPM-o 4.5 (GGUF Q4_K_M) | 9B 全双工多模态模型 |
| 推理引擎 | llama.cpp-omni (libomni.so) | C++ 推理后端，直接链接 |
| 摄像头 | USB / RTSP | OpenCV 或 FFmpeg 拉流 |
| 语音合成 | 内置 CosyVoice2 TTS | 支持声纹克隆 |
| 音频播放 | ALSA (aplay) | HDMI/主板/USB 输出 |
| GUI 窗口 | SDL2 + OpenCV | 摄像头画面显示 |
| 脚本引擎 | LuaJIT | 热更新，业务逻辑可编程 |
| 编程语言 | C++17 | 零 Python 依赖 |

## 已实现功能

- [x] USB 摄像头实时画面采集
- [x] 麦克风收音（ALSA）
- [x] 全双工视频语音推理（直接链接 libomni.so）
- [x] AI 实时描述画面内容
- [x] TTS 语音合成播报（CosyVoice2，HDMI 输出）
- [x] 声纹克隆（参考音频）
- [x] SDL2 窗口显示摄像头画面
- [x] 单二进制部署，零 Python 依赖
- [x] Lua 脚本热更新（业务逻辑可编程）

## 目录结构

```
/opt/friday/
├── chat/
│   ├── gui2                  # 主程序 (C++, 190KB)
│   ├── gui2.cpp              # 源码
│   ├── play_tts.sh           # 音频播放器
│   ├── gui3.cpp              # HTTP 版 (备选)
│   ├── cam_monitor.py        # 摄像头扫描 (测试用)
│   ├── custom_server/        # C++ 工程
│   ├── MiniCPM-o-Demo/       # 官方 Demo
│   └── [design.md](chat/design.md)      # 设计文档
├── README.md
└── LICENSE
```

## 快速运行

```bash
# 启动推理
LD_LIBRARY_PATH=/opt/llama.cpp-omni/build/bin /opt/friday/chat/gui2

# 启动音频播放（新终端）
bash /opt/friday/chat/play_tts.sh
```

## 硬件要求

- **GPU**: NVIDIA, 显存 >= 12GB
- **系统**: Ubuntu 22.04+
- **内存**: >= 32GB
- **CUDA**: >= 12.8
