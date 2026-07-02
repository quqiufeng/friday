# Friday - MiniCPM-o 全双工视频语音系统

> 钢铁侠的 Friday，开源实现。
> 基于 [MiniCPM-o 4.5](https://github.com/OpenBMB/MiniCPM-o) + [llama.cpp-omni](https://github.com/tc-mb/llama.cpp-omni) 构建。

## 1. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│  C++ (gui.cpp) — 主程序                                      │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  SDL2 窗口 — 摄像头画面渲染 + 底部状态栏                │  │
│  │  OpenCV VideoCapture → SDL_Texture → SDL_RenderCopy    │  │
│  │  SDL2_ttf 中文字体渲染                                   │  │
│  └───────────────────────┬───────────────────────────────┘  │
│                          │ 直接 API 调用                      │
│  ┌───────────────────────▼───────────────────────────────┐  │
│  │  libomni.so — 推理层                                   │  │
│  │  omni_init / stream_prefill / stream_decode            │  │
│  │  摄像头帧 + 麦克风音频 → AI 推理 → 文字 + TTS           │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

**零 Python、零 HTTP、零 Web 服务、零 Docker。单二进制部署。**

## 2. 技术栈

| 层级 | 技术 | 职责 |
|------|------|------|
| **渲染层** | C++17 + SDL2 + SDL2_ttf + OpenCV | 30fps 摄像头画面、状态显示 |
| **推理层** | C++ + libomni.so (llama.cpp-omni) | 视觉+音频推理、TTS 生成 |
| **音频** | ALSA (aplay) | 播放 TTS WAV |

## 3. 文件结构

```
/opt/friday/chat/custom_server/
├── gui.cpp              # 主程序 (C++17, ~460 行)
├── gui                  # 编译产物
├── CMakeLists.txt       # CMake 编译配置
├── main.cpp             # C++ + LuaJIT 服务端入口
├── gateway.cpp/.h       # TCP socket 网关
├── lua_bridge.cpp/.h    # LuaJIT C 绑定
├── camera.cpp/.h        # 摄像头 (RTSP)
├── server.lua           # 纯 Lua 推理主循环
└── scripts/
    ├── monitor.lua      # 默认监控脚本 (可热更新)
    └── ws_server.lua    # WebSocket 服务端 (纯 Lua FFI)
```

## 4. 主程序流程 (gui.cpp)

### 4.1 初始化

1. `omni_init` — 加载 LLM + Vision + Audio + TTS 模型
2. 设置 system prompt（控制 AI 行为）
3. 声纹参考音频加载
4. 摄像头初始化（OpenCV VideoCapture, 640x480）
5. 启动摄像头采集线程（~30fps）

### 4.2 推理循环

```
每 200ms:
  1. 检查唤醒词标记
  2. 保存当前帧为 JPEG
  3. 录音 1 秒 (ALSA ffmpeg)
  4. stream_prefill(audio, image)
  5. stream_decode → LLM 生成
  6. 等待 text_queue 输出 (条件变量)
  7. 显示 AI 文字到底部状态栏
  8. 合并本轮 TTS WAV 并播放 (ffmpeg concat + aplay)
```

### 4.3 SDL2 界面

| 区域 | 内容 |
|------|------|
| 主体 | 摄像头画面，等比缩放居中 |
| 底部栏 (110px) | 状态信息 + AI 回复文字 + ESC 退出提示 |

### 4.4 关键环境变量

```bash
export DISPLAY=:0                      # X11 显示
export LIBGL_ALWAYS_SOFTWARE=1         # Mesa software rendering
export OPENCV_LOG_LEVEL=DISABLED       # 禁用 gphoto2 插件警告
export LD_LIBRARY_PATH=/opt/llama.cpp-omni/build/bin  # libomni.so 路径
```

## 5. 编译

```bash
mkdir -p /tmp/build_gui && cd /tmp/build_gui
cmake /opt/friday/chat/custom_server -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cp gui /opt/friday/chat/custom_server/gui
```

## 6. 运行

```bash
LD_LIBRARY_PATH=/opt/llama.cpp-omni/build/bin \
DISPLAY=:0 \
LIBGL_ALWAYS_SOFTWARE=1 \
OPENCV_LOG_LEVEL=DISABLED \
/opt/friday/chat/custom_server/gui
```

## 7. 硬件要求

| 项目 | 最低 | 推荐 |
|------|------|------|
| GPU | 12GB 显存 | RTX 3080 20GB |
| 内存 | 32GB | 64GB |
| 系统 | Ubuntu 22.04+ | Ubuntu 24.04 |
| 摄像头 | USB Camera | 1080p USB Camera |

## 8. 已知问题与解决方案

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| gphoto2 插件 crash | OpenCV 加载 libgphoto2 时符号缺失 | `OPENCV_LOG_LEVEL=DISABLED` |
| EGL 初始化失败 | NVIDIA 驱动 + Mesa 不兼容 | `LIBGL_ALWAYS_SOFTWARE=1` |
| 窗口不显示 | SDL2 需要正确的 DISPLAY | `DISPLAY=:0` |
| GPU 显存不足 | 残留进程占用 | `pkill -9 gui` 后重试 |

## 9. 演进历史

| 阶段 | 方案 | 结果 |
|------|------|------|
| v1 | Python Web Demo (官方) | ✅ 功能完整，依赖重 |
| v2 | C++ SDL2 + libomni (gui.cpp) | ✅ 单二进制，零依赖 |
| v3 | C++ + LuaJIT (custom_server) | ✅ 可热更新脚本 |

## 总结

**C++17 + SDL2 + libomni.so，三位一体。**

```
gui.cpp              → 主程序，SDL2 窗口 + 推理循环
libomni.so           → 本地 AI 推理，零延迟
ffmpeg + aplay        → 音频录制 + TTS 播放
```

零 Python、零 HTTP、零 Docker。跑在你家里的电脑上。
