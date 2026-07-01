#include "camera.h"
#include <cstdio>
#include <thread>
#include <atomic>

static std::atomic<bool> g_cam_running{false};

void camera_stop() { g_cam_running = false; }

bool camera_start(CameraConfig *cfg) {
    if (!cfg || cfg->rtsp_url.empty()) {
        fprintf(stdout, "[摄像头] 未配置 RTSP 地址，跳过\n");
        return true;
    }
    fprintf(stdout, "[摄像头] RTSP 拉流待 Lua 实现: %s\n", cfg->rtsp_url.c_str());
    return true;
}
