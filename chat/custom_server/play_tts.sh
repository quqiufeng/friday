#!/bin/bash
# play_tts.sh - 轮询播放 TTS 生成的 WAV 文件
set -euo pipefail

TTS_DIR="/tmp/omni_out2/tts_wav"
DEVICE="plughw:0,3"
POLL_INTERVAL=0.3

cleanup() {
    echo "[player] 停止播放"
    exit 0
}
trap cleanup SIGINT SIGTERM EXIT

echo "[player] TTS 播放监听启动... (设备=$DEVICE, 目录=$TTS_DIR)"

while true; do
    # 查找最新的 WAV 文件
    latest=$(find "$TTS_DIR" -name "wav_*.wav" -type f -printf '%T@ %p\n' 2>/dev/null \
             | sort -rn \
             | head -1 \
             | cut -d' ' -f2-)

    if [ -n "$latest" ] && [ -f "$latest" ]; then
        aplay -D "$DEVICE" -q "$latest" 2>/dev/null
    else
        sleep "$POLL_INTERVAL"
    fi
done
