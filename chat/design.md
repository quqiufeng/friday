# Friday - MiniCPM-o 全双工视频语音系统

> 钢铁侠的 Friday，开源实现。
> 基于 [MiniCPM-o 4.5](https://github.com/OpenBMB/MiniCPM-o) + [llama.cpp-omni](https://github.com/tc-mb/llama.cpp-omni) 构建。
> 官方在线 Demo: https://minicpmo45.modelbest.cn/omni（浏览器全双工，C++ 版架构参考）
> 官方文档: https://minicpmo45.modelbest.cn/docs/zh/

## 快速启动 (C++ 后端 + Web)

```bash
# 1. 编译 llama-omni-server
cd /opt/llama.cpp-omni/build
cmake .. -DLLAMA_BUILD_SERVER=ON -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES="80;89"
cmake --build . --target llama-omni-server -j$(nproc)

# 2. 启动 llama-omni-server（后端推理）
/opt/llama.cpp-omni/build/bin/llama-omni-server \
  -m /data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf \
  -ngl 99 --host 127.0.0.1 --port 22500

# 3. 启动 Worker（转发请求到 llama-omni-server）
cd /opt/friday/chat/web
/data/venv/bin/python worker.py \
  --host 0.0.0.0 --port 22400 --gpu-id 0 \
  --backend-server-url http://127.0.0.1:22500

# 4. 启动 Gateway（WebSocket 入口）
/data/venv/bin/python gateway.py \
  --host 0.0.0.0 --port 8006 \
  --workers localhost:22400
  # --https --ssl-certfile certs/cert.pem --ssl-keyfile certs/key.pem

# 浏览器打开 https://localhost:8006
```

或一键启动脚本（自动启动后端 + Worker + Gateway，Ctrl+C 停止）：

```bash
bash /opt/friday/chat/custom_server/start.sh
```

日志目录: `/opt/friday/chat/custom_server/logs/`

| 进程 | 端口 | 日志文件 |
|------|------|----------|
| llama-omni-server | 127.0.0.1:22500 | `logs/llama-server.log` |
| Worker | 0.0.0.0:22400 | `logs/worker.log` |
| Gateway | 0.0.0.0:8006 | `logs/gateway.log` |

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
| **音频捕获** | ALSA libasound | snd_pcm_readi 直接 PCM，零子进程 |
| **音频播放** | ALSA aplay | 播放 TTS 合并 WAV |

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
4. 摄像头初始化（OpenCV VideoCapture, 640x480, CAP_V4L2）
5. 启动摄像头采集线程（~30fps）

### 4.2 推理循环 (流式)

```
循环 (ALSA 阻塞读天然同步 ~1s/cycle):
  1. ALSA snd_pcm_readi → PCM 内存 (零子进程)
  2. cv::imwrite → JPEG tmpfs
  3. stream_prefill(audio_file, image_file)
  4. stream_decode → LLM 生成
  5. text_cv.wait_for → 条件变量等 AI 回复
  6. 显示 AI 文字到底部状态栏
  7. play_tts_merge → ffmpeg concat + aplay
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

## 4.5 摄像头设备

| 设备 | 类型 | 使用方法 |
|------|------|----------|
| **USB 摄像头** (带麦克风) | `/dev/video0` | 默认模式，`cap.open(0, CAP_V4L2)` |
| **TP-Link RTSP 摄像头** | `rtsp://admin:密码@IP:554/stream1` | 设置环境变量 `CAMERA_RTSP_URL` |

USB 摄像头内置麦克风作为音频输入源（`plughw:2,0`），与 RTSP 模式共用同一麦克风。

### 切换方式

```bash
# USB 摄像头（默认，不带 CAMERA_RTSP_URL）
/opt/friday/chat/custom_server/gui

# RTSP 摄像头
CAMERA_RTSP_URL='rtsp://admin:wuyou272097579@192.168.1.77:554/stream1' \
/opt/friday/chat/custom_server/gui
```

## 4.6 音频设备一览

| 用途 | 设备 | 描述 |
|------|------|------|
| **麦克风输入 (capture)** | `plughw:2,0` | USB 摄像头内置麦克风 (card 2) |
| **TTS 播放 (playback)** | `plughw:3,0` | USB 音频设备 Rouyin-tianyuan-43198 (card 3) |
| HDMI 音频 | `plughw:0,3` | NVIDIA HDMI (备用播放设备) |
| 板载声卡 | `plughw:1,0` | Realtek ALC892 (Auto-Mute 已禁用) |

```bash
# 查看当前音频设备
aplay -l
arecord -l
```

## 5. 编译

```bash
mkdir -p /tmp/build_gui && cd /tmp/build_gui
cmake /opt/friday/chat/custom_server -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cp gui /opt/friday/chat/custom_server/gui
```

## 5.1 音频设备配置

TTS 设备在源码 `TTS_DEVICE` 中配置：
```cpp
static const char *TTS_DEVICE = "plughw:3,0";  // USB 音频设备
```

### 调试命令

```bash
# 测试 TTS 播放
ffmpeg -y -f lavfi -i "sine=frequency=880:duration=0.2" -af "volume=3" \
  -ac 1 -ar 48000 /tmp/_test.wav 2>/dev/null
aplay -D plughw:3,0 /tmp/_test.wav

# 查看当前音频设备
aplay -l
arecord -l
```

参考设备一览表见 [§4.6 音频设备一览](#46-音频设备一览)。

## 6. 运行

### 基本环境变量
```bash
export LD_LIBRARY_PATH=/opt/llama.cpp-omni/build/bin
export DISPLAY=:0
export LIBGL_ALWAYS_SOFTWARE=1
export OPENCV_LOG_LEVEL=DISABLED
```

### USB 摄像头（默认）
```bash
/opt/friday/chat/custom_server/gui
```

### RTSP 网络摄像头 (TP-Link / 海康等)
```bash
CAMERA_RTSP_URL='rtsp://admin:wuyou272097579@192.168.1.77:554/stream1' \
/opt/friday/chat/custom_server/gui
```

RTSP 地址格式（TP-Link）：
| 码流 | 地址 |
|------|------|
| 主码流 | `rtsp://admin:密码@IP:554/stream1` |
| 子码流 | `rtsp://admin:密码@IP:554/stream2` |

摄像头 RTSP 密码为设备本地管理员密码，非云端账号密码。

USB 摄像头与 RTSP 摄像头通过 `CAMERA_RTSP_URL` 环境变量有无切换，详见 [§4.5 摄像头设备](#45-摄像头设备)。

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

## 9. 架构参考: Duplex 双工机制 (开发指导)

官方文档: https://minicpmo45.modelbest.cn/docs/zh/architecture/duplex/

### 9.1 核心概念

Duplex 模式实现了类似电话的实时对话体验：用户说话的同时，模型可随时开口回应。

| 特性 | Chat 模式 | Half-Duplex 模式 | Duplex 模式 |
|------|-----------|-----------------|-------------|
| 交互方式 | 轮次式（手动触发） | 轮次式（VAD 自动触发） | 实时全双工（同时收听和回应） |
| 输入处理 | 一次性 prefill | VAD 检测 → prefill | 每秒流式 prefill 音频/视频 |
| Worker 占用 | 仅推理期间 | 整个会话独占 | 整个会话独占 |

### 9.2 每秒一次的 Unit 循环

Duplex 核心是一个**每秒执行一次**的 prefill-generate 循环，每次循环称为一个 "unit"：

```
loop 每秒一个 Unit:
  客户端 → 服务端: audio_chunk (~1s) + video_frame
  服务端: streaming_prefill()
    1. feed ⟨unit⟩ token（标记新 unit 开始）
    2. 编码图像 → feed 视觉 embedding
    3. 编码音频 → feed 音频 embedding
    4. 产生 pending_logits
  服务端: streaming_generate()
    基于 pending_logits 解码
    输出 ⟨listen⟩ → 继续聆听
    输出文本 token → 说话
  alt 模型决定说话:
    服务端 → 客户端: text + audio_data
  else 模型决定聆听:
    服务端 → 客户端: is_listen=true
```

关键代码逻辑:
```python
# streaming_prefill(): 每秒调用一次
self.decoder.feed(self.decoder.embed_token(self.unit_token_id))
vision_hidden_states = self.model.get_vision_embedding(processed_frames)
self.decoder.feed(vision_embed)
audio_embeds = self.model.get_audio_embedding(processed_audio)
self.decoder.feed(audio_embed)
# → 产生 pending_logits

# streaming_generate(): 基于 pending_logits 解码
logits = self.pending_logits
for j in range(max_new_speak_tokens_per_chunk):
    last_id = self.decoder.decode(logits=logits, mode=decode_mode, ...)
    if last_id == listen_token_id:
        break  # 模型选择聆听
    elif last_id in chunk_terminator_token_ids:
        break  # chunk 结束
    else:
        self.res_ids.append(last_id.item())  # 说话 token
        logits, hidden = self.decoder.feed(...)  # 继续解码
```

### 9.3 完整流程（含 Gateway 代理）

```
客户端 → Gateway: WS /ws/duplex/{session_id}
Gateway → Queue: enqueue("omni_duplex")
Queue → Worker: 分配 Worker（独占）
客户端 → Gateway: prepare (系统提示词 + 配置)
Gateway → Worker: duplex_prepare()
loop 全双工循环（每秒一次）:
  客户端 → Gateway: audio_chunk (+ video_frame)
  Gateway → Worker: duplex_prefill(audio, frames)
  Worker → Worker: duplex_generate()
  alt 模型决定说话:
    Worker → Gateway → 客户端: result (text + audio_data)
  else:
    Worker → Gateway → 客户端: result (is_listen=true)
客户端 → Gateway: stop
Gateway → Worker: duplex_cleanup()
Gateway → Queue: release_worker()
```

### 9.4 Omnimodal vs Audio 模式

| 模式 | 方式 | 输入 |
|------|------|------|
| **Omnimodal Full-Duplex** | `mode=video` | audio_chunk + video_frame（每秒） |
| **Audio Full-Duplex** | `mode=audio` | 仅 audio_chunk（每秒） |

两者共享相同的 prefill-generate unit 循环，区别仅在于是否传入视频帧。

## 10. Realtime API 协议

官方文档: https://minicpmo45.modelbest.cn/docs/zh/realtime-api/

### 10.1 视频双工 (`/v1/realtime?mode=video`)

视频全双工: 客户端持续发送 16kHz 音频 + JPEG 帧; 服务端返回 listen/text/audio。

**连接**

```
wss://host/v1/realtime?mode=video
```

| 项目 | 值 |
|------|-----|
| 帧格式 | JSON 文本帧 |
| 上行音频 | 16kHz, 单声道, float32 PCM, base64 |
| 上行视频 | JPEG base64 (`input.video_frames`) |
| 下行音频 | 24kHz, 单声道, float32 PCM, base64 |
| 会话上限 | 300 秒 |

**初始化**

```
→ session.init
{
  "type": "session.init",
  "payload": {
    "system_prompt": "你是一个有用的视频语音助手",
    "config": { "length_penalty": 1.1 },
    "voice": {
      "ref_audio_base64": "<base64 float32 PCM>",
      "tts_ref_audio_base64": "<base64 float32 PCM>"
    }
  }
}

← session.created
```

**发送输入**

```
→ input.append
{
  "type": "input.append",
  "input": {
    "audio": "<base64 float32 PCM, 16kHz mono>",
    "video_frames": ["<base64 JPEG>"],
    "force_listen": false,
    "max_slice_nums": 1
  }
}
```

**接收输出**

| kind | 说明 |
|------|------|
| `listen` | 模型回到听状态，可继续发送输入 |
| `text` | 文本增量 (`delta.text`) |
| `audio` | 音频增量 (`delta.audio`, 24kHz float32 PCM base64) |

```
← response.output.delta { "kind": "listen", "session_id": "sess_xxx", "metrics": {...} }
← response.output.delta { "kind": "text",   "text": "我看到了", "response_id": "resp_xxx" }
← response.output.delta { "kind": "audio",  "audio": "<base64>", "response_id": "resp_xxx" }
```

输出边界由 `kind=listen` 表达，不使用 `response.done`。

**时序**

```
connect → session.queued/queue_done → session.init → session.created
  → input.append(audio+video) 循环
  ← response.output.delta (listen / text / audio)
  → session.close ← session.closed
```

### 10.2 音频双工 (`/v1/realtime?mode=audio`)

纯语音双工, 无视频帧, 会话上限 600 秒。

**连接**

```
wss://host/v1/realtime?mode=audio
```

**初始化** (同视频双工，无 video_frames)

```
→ session.init
{
  "type": "session.init",
  "payload": {
    "system_prompt": "你是一个有用的语音助手",
    "config": { "length_penalty": 1.1 },
    "voice": { "ref_audio_base64": "...", "tts_ref_audio_base64": "..." }
  }
}

← session.created
```

**发送输入** (无 video_frames)

```
→ input.append
{
  "type": "input.append",
  "input": {
    "audio": "<base64 float32 PCM, 16kHz mono>",
    "force_listen": false
  }
}
```

**时序** (同视频双工，不携带帧)

### 10.3 协议要点

| 要点 | 说明 |
|------|------|
| 状态判断 | `kind=listen` 表示模型在听, 可发新输入; `kind=text/audio` 表示模型在说 |
| 关闭 | `session.close {reason}` → 服务端回复 `session.closed` |
| 音频格式 | 上行 16kHz float32 PCM mono; 下行 24kHz float32 PCM mono |
| 视频格式 | JPEG base64, 每帧 1 个切片 (`max_slice_nums=1`) |
| 参考音频 | `ref_audio_base64` 给 LLM 做声纹; `tts_ref_audio_base64` 给 TTS (可复用) |

### 10.4 C++ 实现对比

| 环节 | Web (Realtime API) | C++ 单二进制 (gui.cpp) |
|------|-------------------|----------------------|
| 音频捕获 | AudioWorklet → base64 | ALSA `snd_pcm_readi` → PCM → WAV |
| 画面捕获 | canvas → base64 JPEG | OpenCV `cv::imwrite` → JPEG |
| 推理调用 | WebSocket JSON → gateway → worker | 直接 `stream_prefill()` + `stream_decode()` |
| 状态管理 | `kind=listen` / `kind=text` / `kind=audio` | `text_cv.wait_for` 条件变量 |
| TTS 播放 | base64 PCM → AudioContext | `ffmpeg concat` + `aplay` |

C++ 版直接调用 libomni API，省去 WebSocket + HTTP 开销，协议逻辑等价。

## 11. llama-omni-server 编译与启动

C++ 双工推理服务端, 提供 HTTP API 供 Python worker 调用。

### 11.1 本地修改

#### `tools/server/server-omni.cpp` — SSL 降级

修复 OpenSSL 编译下无 cert/key 时 `SSLServer::is_valid()` 返回 false 导致 `listen()` 失败的问题。

```cpp
// 修改后: 无 cert/key 时自动降级为 plain HTTP
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    const bool provide_ssl = params.ssl_file_cert.size() && params.ssl_file_key.size();
    httplib::SSLServer svr_ssl(params.ssl_file_cert.c_str(), params.ssl_file_key.c_str());
    httplib::Server svr_http;
    httplib::Server & svr = (provide_ssl && svr_ssl.is_valid()) ? svr_ssl : svr_http;
    if (!provide_ssl) { LOG_INF("SSL cert not provided, using plain HTTP\n"); }
#else
    httplib::Server svr;
#endif
```

#### `tools/omni/omni.cpp` — 移除 duplex 模式下的英文 prompt 硬编码

`omni_init()` 和 `omni_set_language()` 在 `duplex_mode=true` 时会将 prompt 覆盖为英文 `"Streaming Duplex Conversation! You are a helpful assistant."`，导致调用方设置的中文 prompt 失效。

修改：双工模式下不再覆盖 prompt，由调用方在 `omni_init` 之后自行设置。

```cpp
// 原代码 (omni_init, 约 line 4061):
if (duplex_mode) {
    ctx_omni->omni_voice_clone_prompt = "...Streaming Duplex Conversation...";
    ctx_omni->omni_assistant_prompt   = "...";
}

// 修改后: 双工模式不覆盖 prompt
if (duplex_mode) {
    // 由调用方在 omni_init 之后设置 omni_voice_clone_prompt
}
```

同同理修改 `omni_set_language()` 中 `duplex_mode` 分支。

### 11.2 编译

```bash
# 源码位置: /opt/llama.cpp-omni
cd /opt/llama.cpp-omni/build

# 配置（启用 CUDA + Server）
cmake .. \
  -DLLAMA_BUILD_SERVER=ON \
  -DGGML_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES="80;89"

# 编译 llama-omni-server（仅增量编译该目标）
cmake --build . --target llama-omni-server -j$(nproc)

# 产物: build/bin/llama-omni-server
```

### 11.2 模型目录结构

```
/data/models/MiniCPM-o-4_5-gguf/
├── MiniCPM-o-4_5-Q4_K_M.gguf       # 主模型
├── vision/
│   └── MiniCPM-o-4_5-vision-F16.gguf
├── audio/
│   └── MiniCPM-o-4_5-audio-F16.gguf
├── tts/
│   ├── MiniCPM-o-4_5-tts-F16.gguf
│   └── MiniCPM-o-4_5-projector-F16.gguf
└── token2wav-gguf/
    ├── encoder.gguf
    ├── flow_extra.gguf
    ├── flow_matching.gguf
    ├── hifigan2.gguf
    └── prompt_cache.gguf
```

### 11.3 启动服务

```bash
# 启动 llama-omni-server (后端推理, 127.0.0.1:22500)
/opt/llama.cpp-omni/build/bin/llama-omni-server \
  -m /data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf \
  -ngl 99 \
  --host 127.0.0.1 \
  --port 22500

# 启动 Python worker (转发到后端, 0.0.0.0:22400)
cd /opt/friday/chat/web
/data/venv/bin/python worker.py \
  --host 0.0.0.0 \
  --port 22400 \
  --gpu-id 0 \
  --backend-server-url http://127.0.0.1:22500

# 启动 Gateway (WebSocket 入口, 0.0.0.0:8006)
/data/venv/bin/python gateway.py \
  --host 0.0.0.0 \
  --port 8006 \
  --workers cpp-worker-backend:22400
  # 可选 --https --ssl-certfile certs/cert.pem --ssl-keyfile certs/key.pem
```

### 11.4 Docker 部署（参考）

```bash
cd /opt/friday/chat/web
GGUF_MODEL_HOST_PATH=/data/models/MiniCPM-o-4_5-gguf \
GATEWAY_HOST_PORT=8006 \
CPP_GPU_ID=0 \
docker compose -f docker-compose.cpp.yml up -d --build
```

## 12. 演进历史

| 阶段 | 方案 | 结果 |
|------|------|------|
| v1 | Python Web Demo (官方) | ✅ 功能完整，依赖重 |
| v2 | C++ SDL2 + libomni (gui.cpp) | ✅ 单二进制，零依赖 |
| v3 | C++ + LuaJIT (custom_server) | ✅ 可热更新脚本 |

## 13. LuaJIT 脚本引擎

### 13.1 编译集成

```cmake
pkg_check_modules(LUAJIT REQUIRED luajit)
include_directories(${LUAJIT_INCLUDE_DIRS})
target_link_libraries(gui PRIVATE ${LUAJIT_LIBRARIES})
```

LuaJIT 位于 `/usr/local/luajit`，cjson.so 在 `/usr/local/lualib/`。

### 13.2 调用方式

每次 MiniCPM-o 回复完整后被调用：

```cpp
// gui.cpp 中, 模型说完话后自动调用
lua_getglobal(L, "on_reply");
lua_pushstring(L, speak_buf.c_str());  // reply_text: 模型完整回复
lua_pushstring(L, "/home/quqiufeng/friday.txt");  // log_path: 日志文件路径
lua_pcall(L, 2, 0, 0);
```

### 13.3 Lua 脚本

`scripts/friday.lua`（热更新，改完自动重载）：

```lua
-- 默认实现：打印回复日志
function on_reply(reply_text, log_path)
    print("[Friday] 收到回复: " .. reply_text:sub(1, 80) .. "...")
    -- 可扩展：转发到 opencode 或其他模型 API
end
```

### 13.4 热更新机制

```cpp
// 每次 on_reply 调用前检查 mtime，变化则自动 reload
struct stat st;
if (stat("scripts/friday.lua", &st) == 0 && st.st_mtime > lua_mtime) {
    lua_mtime = st.st_mtime;
    load_lua();  // 重新加载
}
```

## 总结

**C++17 + SDL2 + libomni.so，三位一体。**

```
gui.cpp              → 主程序，SDL2 窗口 + 推理循环
libomni.so           → 本地 AI 推理，零延迟
ffmpeg + aplay        → 音频录制 + TTS 播放
```

零 Python、零 HTTP、零 Docker。跑在你家里的电脑上。
