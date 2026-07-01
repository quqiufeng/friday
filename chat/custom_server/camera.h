#pragma once
#include <string>
#include <functional>

struct CameraConfig {
    std::string rtsp_url;
    int width = 640;
    int quality = 70;
    std::function<void(const unsigned char *jpeg, size_t len, double timestamp)> on_frame;
};

bool camera_start(CameraConfig *cfg);
void camera_stop();
