#!/bin/bash
set -e

echo "[start] 启动 llama-server..."
nohup /opt/llama.cpp-omni/build/bin/llama-server \
  --host 127.0.0.1 --port 19080 \
  --model /data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf \
  --ctx-size 8192 --n-gpu-layers 99 \
  --repeat-penalty 1.05 --temp 0.7 \
  > /tmp/llama-server.log 2>&1 &

echo "[start] 等待 llama-server 就绪..."
for i in $(seq 1 60); do
  if curl -s http://127.0.0.1:19080/health >/dev/null 2>&1; then
    echo "[start] llama-server 就绪"
    break
  fi
  sleep 5
done

echo "[start] 调用 omni_init..."
curl -s http://127.0.0.1:19080/v1/stream/omni_init \
  -X POST \
  -H "Content-Type: application/json" \
  -d '{
    "media_type": 2,
    "use_tts": true,
    "duplex_mode": true,
    "model_dir": "/data/models/MiniCPM-o-4_5-gguf",
    "tts_bin_dir": "/data/models/MiniCPM-o-4_5-gguf/tts",
    "tts_gpu_layers": 100,
    "token2wav_device": "gpu:0",
    "output_dir": "/tmp/omni_out2",
    "voice_clone_prompt": "<|im_start|>system\n你是一个监控管理员，持续观察摄像头画面。正常情况下保持静默观察，不要主动说话。发现异常情况时简洁描述当前画面。\n<|audio_start|>",
    "assistant_prompt": "<|audio_end|><|im_end|>\n"
  }'

echo "[start] omni_init 完成，启动 GUI..."
cd /opt/friday/chat/gui_gpui
OPENCV_LOG_LEVEL=DISABLED LIBGL_ALWAYS_SOFTWARE=1 \
LD_LIBRARY_PATH=/opt/friday/chat/gui_gpui/target/release:/opt/friday/chat/gui_gpui \
exec ./friday_launcher
