#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <signal.h>

#include "camera.h"
#include "gateway.h"
#include "lua_bridge.h"

// ─── 全局状态 ──────────────────────────────────────────────────────
static bool g_running = true;

// ─── 参数 ──────────────────────────────────────────────────────────
static std::string g_model_path;
static int g_port = 8040;
static int g_n_ctx = 8192;
static int g_n_gpu_layers = 99;
static std::string g_rtsp_url;
static std::string g_script_path = "../scripts/ws_server.lua";

static void print_usage() {
    fprintf(stderr,
        "Custom Server - MiniCPM-o 监控服务器 (C++ + LuaJIT)\n"
        "\n"
        "用法:\n"
        "  custom_server --model <path> [选项]\n"
        "\n"
        "必选:\n"
        "  --model <path>          GGUF 模型文件路径\n"
        "\n"
        "可选:\n"
        "  --port <port>           HTTP 端口 (默认: 8040)\n"
        "  --rtsp <url>            RTSP 摄像头地址\n"
        "  --script <path>         Lua 脚本路径\n"
        "  --ctx-size <n>          上下文大小 (默认: 8192)\n"
        "  --n-gpu-layers <n>      GPU offload 层数 (默认: 99)\n"
        "  --help                  显示此帮助\n"
    );
}

static bool parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            exit(0);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            g_model_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ctx-size") == 0 && i + 1 < argc) {
            g_n_ctx = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--n-gpu-layers") == 0 && i + 1 < argc) {
            g_n_gpu_layers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rtsp") == 0 && i + 1 < argc) {
            g_rtsp_url = argv[++i];
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            g_script_path = argv[++i];
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return false;
        }
    }
    if (g_model_path.empty()) {
        fprintf(stderr, "错误: --model 是必选参数\n");
        return false;
    }
    return true;
}

static void signal_handler(int) {
    g_running = false;
}

int main(int argc, char **argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!parse_args(argc, argv)) {
        print_usage();
        return 1;
    }

    fprintf(stdout, "\n");
    fprintf(stdout, "========================================\n");
    fprintf(stdout, "  Custom Server - MiniCPM-o 监控服务器\n");
    fprintf(stdout, "========================================\n");
    fprintf(stdout, "  模型: %s\n", g_model_path.c_str());
    fprintf(stdout, "  端口: %d\n", g_port);
    fprintf(stdout, "  GPU:  %d 层\n", g_n_gpu_layers);
    fprintf(stdout, "  RTSP: %s\n", g_rtsp_url.empty() ? "无" : g_rtsp_url.c_str());
    fprintf(stdout, "  脚本: %s\n", g_script_path.c_str());
    fprintf(stdout, "========================================\n\n");

    // 初始化 LuaJIT
    lua_init();
    lua_load_script(g_script_path);

    // 启动 HTTP 服务
    gateway_start(g_port);

    // 主循环
    while (g_running) {
        lua_call_tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    fprintf(stdout, "\n[关闭] 正在停止...\n");
    gateway_stop();
    lua_close();

    return 0;
}
