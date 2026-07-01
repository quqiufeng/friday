# Friday - MiniCPM-o 4.5 全双工视频语音监控系统

> 钢铁侠的 Friday，开源实现。
> 基于 [MiniCPM-o 4.5](https://github.com/OpenBMB/MiniCPM-o) + [llama.cpp-omni](https://github.com/tc-mb/llama.cpp-omni) 构建。

## 1. 架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│  Rust (SDL2 + OpenCV) — 渲染层                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  60fps 摄像头画面渲染, SDL2 窗口, 深色主题                  │  │
│  │  OpenCV VideoCapture → SDL_Texture → SDL_RenderCopy        │  │
│  └───────────────────────┬───────────────────────────────────┘  │
│                          │ FFI (C ABI)                          │
│  ┌───────────────────────▼───────────────────────────────────┐  │
│  │  C (main.c) — 启动器                                       │  │
│  │  dlopen(libfriday_gui.so) → gui_app_create → gui_run       │  │
│  └───────────────────────┬───────────────────────────────────┘  │
│                          │                                       │
│  ┌───────────────────────▼───────────────────────────────────┐  │
│  │  C++ (libomni.so) — 推理层                                 │  │
│  │  omni_init / stream_prefill / stream_decode                │  │
│  │  摄像头帧 + 麦克风音频 → AI 推理 → 文字 + TTS               │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

**零 Python、零 HTTP、零 Web 服务、零 Docker、零 npm。**

## 2. 技术栈

| 层级 | 技术 | 职责 | 产物 |
|------|------|------|------|
| **渲染层** | Rust + SDL2 + OpenCV | 60fps 摄像头画面、状态显示 | `libfriday_gui.so` |
| **启动器** | C (main.c) | dlopen 加载 Rust .so，调用 FFI | `friday_launcher` |
| **推理层** | C++ + libomni.so | 视觉+音频推理、TTS 生成 | `libomni.so` (1.7MB) |
| **脚本层** | Bash | 唤醒词检测、TTS 播放 | `wake.sh` / `play_tts.sh` |

## 3. 文件结构

```
/opt/friday/chat/
├── gui_gpui/                        # Rust SDL2 渲染层
│   ├── Cargo.toml                   # sdl2 + opencv
│   ├── src/
│   │   └── lib.rs                   # 摄像头采集 + SDL2 渲染 + FFI
│   ├── main.c                       # C 启动器 (dlopen)
│   ├── friday_launcher              # 编译后的启动器
│   └── target/release/
│       └── libfriday_gui.so         # 编译产物
│
├── custom_server/                   # 辅助脚本
│   ├── wake.sh                      # "你好 星期五" 唤醒词检测
│   ├── play_tts.sh                  # TTS WAV 播放 (HDMI)
│   └── cam_monitor.lua              # 局域网摄像头扫描推流
│
├── wake_confirm.wav                 # 唤醒提示音
└── design.md                        # 本文档
```

## 4. 渲染层设计 (Rust SDL2)

### 4.1 核心逻辑

```rust
// 摄像头线程: OpenCV VideoCapture → RGB 帧
fn camera_thread(frame: Arc<Mutex<Option<(u32, u32, Vec<u8>)>>>) {
    let mut cap = VideoCapture::new(0, CAP_ANY).unwrap();
    loop {
        cap.read(&mut mat);
        cvt_color(&mat, &mut rgb, COLOR_BGR2RGB);
        *frame.lock() = Some((w, h, rgb_data));
        sleep(10ms); // ~100fps 采集
    }
}

// 主线程: SDL2 渲染
fn gui_run() {
    let sdl = sdl2::init().unwrap();
    let win = sdl.window("Friday", 960, 640);
    let mut canvas = win.into_canvas().unwrap();
    let tex = canvas.texture_creator().create_texture_streaming(BGR24, 640, 480);

    loop {
        // 事件处理
        for event in events.poll_iter() { ... }

        // 渲染摄像头帧
        if let Some((w, h, data)) = frame.lock().take() {
            tex.update(None, &data, w * 3);
            canvas.copy(&tex, None, None);
        }
        canvas.present();
        sleep(10ms); // 60fps 渲染
    }
}
```

### 4.2 FFI 接口

```c
void* gui_app_create(const char* config);  // 创建应用 (启动摄像头线程)
int   gui_run(void* app);                   // 启动 SDL2 事件循环 (阻塞)
void  gui_stream_delta(void* app, text);    // AI 文字更新
void  gui_set_status(void* app, text);      // 状态文字更新
void  gui_app_free(void* app);              // 释放资源
```

### 4.3 关键环境变量

```bash
export DISPLAY=:0                      # X11 显示
export LIBGL_ALWAYS_SOFTWARE=1         # Mesa software rendering
export OPENCV_LOG_LEVEL=DISABLED       # 禁用 gphoto2 插件警告
```

## 5. 编译

```bash
cd /opt/friday/chat/gui_gpui
cargo build --release
gcc -o friday_launcher main.c -ldl -lpthread
```

## 6. 运行

```bash
cd /opt/friday/chat/gui_gpui
./friday_launcher
```

一键启动（含唤醒词 + TTS）：
```bash
cd /opt/friday/chat/gui_gpui
nohup ./friday_launcher > /tmp/friday.log 2>&1 & disown
nohup bash ../custom_server/wake.sh > /tmp/wake.log 2>&1 & disown
nohup bash ../custom_server/play_tts.sh > /tmp/player.log 2>&1 & disown
```

## 7. 硬件要求

| 项目 | 最低 | 推荐 |
|------|------|------|
| GPU | 12GB 显存 | RTX 3080 20GB |
| 内存 | 32GB | 64GB |
| 系统 | Ubuntu 22.04+ | Ubuntu 24.04 |
| 摄像头 | USB Camera / RTSP | 1080p USB Camera |

## 8. 已知问题与解决方案

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| gphoto2 插件 crash | OpenCV 加载 libgphoto2 时符号缺失 | `OPENCV_LOG_LEVEL=DISABLED` |
| EGL 初始化失败 | NVIDIA 驱动 + Mesa 不兼容 | `LIBGL_ALWAYS_SOFTWARE=1` |
| 窗口不显示 | SDL2 需要正确的 DISPLAY | `DISPLAY=:0` |

## 9. 演进历史

| 阶段 | 方案 | 结果 |
|------|------|------|
| v1 | C++ SDL2 + OpenCV (gui3.cpp) | ✅ 能用 |
| v2 | Rust GPUI (Zed) | ❌ EGL 初始化失败 |
| v3 | Rust SDL2 + OpenCV | ✅ 能用，60fps |

**最终选择 Rust SDL2**：与 gui3 相同的底层（SDL2 + OpenCV + X11），但用 Rust 提供内存安全和更好的可维护性。

## 总结

**Rust SDL2 渲染 + C 启动器 + C++ 推理，三位一体。**

```
Rust (SDL2 + OpenCV)  → 60fps 摄像头画面，内存安全
C (main.c)            → 轻量启动器，dlopen 动态加载
C++ (libomni.so)      → 本地 AI 推理，零延迟
```

零 Python、零 HTTP、零 Docker。跑在你家里的电脑上。
