#!/bin/bash
set -euo pipefail

# ─── 配置 ───────────────────────────────────────────────
LLAMA_SERVER="/opt/llama.cpp-omni/build/bin/llama-omni-server"
GGUF_MODEL="/data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf"
BACKEND_HOST="127.0.0.1"
BACKEND_PORT="22500"
WORKER_PORT="22400"
GATEWAY_PORT="8006"
N_GPU_LAYERS="99"
VENV_PYTHON="/data/venv/bin/python"
WEB_DIR="/opt/friday/chat/web"
LOG_DIR="/opt/friday/chat/custom_server/logs"

# ─── 颜色 ───────────────────────────────────────────────
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }

# ─── 清理 ───────────────────────────────────────────────
cleanup() {
    info "停止所有进程..."
    [ -n "${worker_pid:-}" ] && kill "$worker_pid" 2>/dev/null || true
    [ -n "${gateway_pid:-}" ] && kill "$gateway_pid" 2>/dev/null || true
    [ -n "${backend_pid:-}" ] && kill "$backend_pid" 2>/dev/null || true
    wait 2>/dev/null || true
    info "已停止"
    exit 0
}
trap cleanup SIGTERM SIGINT

mkdir -p "$LOG_DIR"

# ─── 1. 启动 llama-omni-server ──────────────────────────
info "启动 llama-omni-server (端口 ${BACKEND_PORT})..."
"$LLAMA_SERVER" \
    -m "$GGUF_MODEL" \
    -ngl "$N_GPU_LAYERS" \
    --host "$BACKEND_HOST" \
    --port "$BACKEND_PORT" \
    -c 8192 \
    >> "${LOG_DIR}/llama-server.log" 2>&1 &
backend_pid=$!

info "等待后端就绪..."
max_retries=300
for i in $(seq 1 "$max_retries"); do
    if ! kill -0 "$backend_pid" 2>/dev/null; then
        error "llama-omni-server 异常退出"
        tail -20 "${LOG_DIR}/llama-server.log" >&2
        exit 1
    fi
    if curl -sf "http://${BACKEND_HOST}:${BACKEND_PORT}/health" >/dev/null 2>&1; then
        info "后端就绪 (~${i}s)"
        break
    fi
    if [ "$i" -eq "$max_retries" ]; then
        error "后端启动超时"
        tail -50 "${LOG_DIR}/llama-server.log" >&2
        exit 1
    fi
    sleep 1
done

# ─── 2. 启动 Worker ─────────────────────────────────────
info "启动 Worker (端口 ${WORKER_PORT})..."
cd "$WEB_DIR"
$VENV_PYTHON worker.py \
    --host 0.0.0.0 \
    --port "$WORKER_PORT" \
    --gpu-id 0 \
    --backend-server-url "http://${BACKEND_HOST}:${BACKEND_PORT}" \
    >> "${LOG_DIR}/worker.log" 2>&1 &
worker_pid=$!
sleep 3

if ! kill -0 "$worker_pid" 2>/dev/null; then
    error "Worker 启动失败"
    tail -20 "${LOG_DIR}/worker.log" >&2
    exit 1
fi
info "Worker 已启动"

# ─── 3. 启动 Gateway ────────────────────────────────────
info "启动 Gateway (端口 ${GATEWAY_PORT})..."
$VENV_PYTHON gateway.py \
    --host 0.0.0.0 \
    --port "$GATEWAY_PORT" \
    --http \
    --workers "localhost:${WORKER_PORT}" \
    >> "${LOG_DIR}/gateway.log" 2>&1 &
gateway_pid=$!
sleep 2

if ! kill -0 "$gateway_pid" 2>/dev/null; then
    error "Gateway 启动失败"
    tail -20 "${LOG_DIR}/gateway.log" >&2
    exit 1
fi
info "Gateway 已启动"

# ─── 就绪 ───────────────────────────────────────────────
echo ""
echo "================================================"
info "全部就绪!"
echo "  llama-omni-server:  http://${BACKEND_HOST}:${BACKEND_PORT}"
echo "  Worker:             http://0.0.0.0:${WORKER_PORT}"
echo "  Gateway:            http://0.0.0.0:${GATEWAY_PORT}"
echo ""
info "浏览器打开: http://localhost:${GATEWAY_PORT}"
echo ""
info "停止: Ctrl+C"
echo "================================================"

while true; do
    if ! kill -0 "$backend_pid" 2>/dev/null; then
        error "llama-omni-server 已退出"
        cleanup
    fi
    if ! kill -0 "$worker_pid" 2>/dev/null; then
        error "Worker 已退出"
        cleanup
    fi
    if ! kill -0 "$gateway_pid" 2>/dev/null; then
        error "Gateway 已退出"
        cleanup
    fi
    sleep 5
done
