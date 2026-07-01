#!/bin/bash
# wake.sh — "你好 星期五" 唤醒词检测
set -euo pipefail

WAKE_MODEL="/data/models/sense-voice-small-q4_k.gguf"
SENSE="/opt/SenseVoice.cpp/build/bin/sense-voice-main"
MIC="plughw:2,0"
AUDIO_CONFIRM="/opt/friday/chat/wake_confirm.wav"
WAKE_FLAG="/tmp/wake_flag"
WAKE_AUDIO="/tmp/_wake_check.wav"
WAKE_RESULT="/tmp/_wake_result.txt"

# 音量阈值 (dB)
VOL_THRESHOLD=-35
# 录音时长 (秒)
RECORD_SEC=2

cleanup() {
    rm -f "$WAKE_AUDIO" "$WAKE_RESULT"
    exit 0
}
trap cleanup SIGINT SIGTERM EXIT

echo "[wake] 唤醒词监听启动... (MIC=$MIC)"

while true; do
    # 录音
    if ! ffmpeg -f alsa -ac 1 -ar 16000 -i "$MIC" -t "$RECORD_SEC" -y "$WAKE_AUDIO" 2>/dev/null; then
        sleep 0.5
        continue
    fi

    # 检查录音文件大小
    filesize=$(stat -c%s "$WAKE_AUDIO" 2>/dev/null || echo 0)
    if [ "$filesize" -lt 1000 ]; then
        sleep 0.5
        continue
    fi

    # 音量检测
    vol=$(ffmpeg -i "$WAKE_AUDIO" -af volumedetect -f null /dev/null 2>&1 \
          | grep mean_volume \
          | grep -oP 'mean_volume:\s*\K[-0-9.]+' \
          || echo "-100")

    if [ -z "$vol" ]; then
        sleep 0.5
        continue
    fi

    # 浮点比较
    if ! echo "$vol $VOL_THRESHOLD" | awk '{exit !($1 > $2)}'; then
        sleep 0.3
        continue
    fi

    echo "[wake] volume=$vol dB, 运行 SenseVoice..." >&2

    # 语音识别
    if ! "$SENSE" -m "$WAKE_MODEL" "$WAKE_AUDIO" > "$WAKE_RESULT" 2>/dev/null; then
        echo "[wake] SenseVoice 执行失败" >&2
        sleep 0.5
        continue
    fi

    # 检查识别结果是否包含"你好"（排除语言标签行）
    if grep -v '\[.*\]' "$WAKE_RESULT" 2>/dev/null | grep -q "你好"; then
        echo "[wake] 检测到唤醒词！" >&2
        date +%s > "$WAKE_FLAG"
        ffplay -nodisp -autoexit -loglevel quiet "$AUDIO_CONFIRM" 2>/dev/null &
        # 防止连续触发，等待 3 秒
        sleep 3
    fi

    sleep 0.5
done
