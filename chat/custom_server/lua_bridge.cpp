#include "lua_bridge.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

static lua_State *g_L = nullptr;
static std::mutex g_lua_mutex;

// ─── V4L2 摄像头 ──────────────────────────────────────────────────
struct CameraDevice {
    int fd = -1;
    void *buf_start = nullptr;
    size_t buf_length = 0;
};

static CameraDevice g_cam;

static bool cam_open(const char *dev) {
    g_cam.fd = open(dev, O_RDWR);
    if (g_cam.fd < 0) { perror("open"); return false; }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(g_cam.fd, VIDIOC_S_FMT, &fmt) < 0) {
        // JPEG not supported, try MJPEG
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        if (ioctl(g_cam.fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT");
            close(g_cam.fd);
            g_cam.fd = -1;
            return false;
        }
    }

    struct v4l2_requestbuffers req = {};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_cam.fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        close(g_cam.fd);
        g_cam.fd = -1;
        return false;
    }

    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(g_cam.fd, VIDIOC_QUERYBUF, &buf) < 0) {
        perror("VIDIOC_QUERYBUF");
        close(g_cam.fd);
        g_cam.fd = -1;
        return false;
    }

    g_cam.buf_length = buf.length;
    g_cam.buf_start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, g_cam.fd, buf.m.offset);
    if (g_cam.buf_start == MAP_FAILED) {
        perror("mmap");
        close(g_cam.fd);
        g_cam.fd = -1;
        return false;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_cam.fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        munmap(g_cam.buf_start, g_cam.buf_length);
        close(g_cam.fd);
        g_cam.fd = -1;
        return false;
    }

    fprintf(stdout, "[摄像头] %s 已打开 (640x480 JPEG)\n", dev);
    return true;
}

static bool cam_capture(std::vector<unsigned char> &jpeg_out) {
    if (g_cam.fd < 0) return false;

    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (ioctl(g_cam.fd, VIDIOC_QBUF, &buf) < 0) return false;
    if (ioctl(g_cam.fd, VIDIOC_DQBUF, &buf) < 0) return false;

    jpeg_out.assign((unsigned char*)g_cam.buf_start,
                    (unsigned char*)g_cam.buf_start + buf.bytesused);
    return true;
}

static void cam_close() {
    if (g_cam.fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_cam.fd, VIDIOC_STREAMOFF, &type);
        if (g_cam.buf_start) munmap(g_cam.buf_start, g_cam.buf_length);
        close(g_cam.fd);
        g_cam.fd = -1;
    }
}

// ─── Lua 绑定函数 ──────────────────────────────────────────────────
static int l_camera_open(lua_State *L) {
    const char *dev = luaL_optstring(L, 1, "/dev/video0");
    lua_pushboolean(L, cam_open(dev));
    return 1;
}

static int l_camera_capture(lua_State *L) {
    std::vector<unsigned char> jpeg;
    if (cam_capture(jpeg)) {
        lua_pushlstring(L, (const char*)jpeg.data(), jpeg.size());
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_camera_close(lua_State *L) {
    cam_close();
    return 0;
}

static int l_alert_save_snapshot(lua_State *L) {
    // 支持传入 JPEG 数据保存
    size_t len;
    const char *data = luaL_optlstring(L, 1, nullptr, &len);
    if (data && len > 0) {
        char path[256];
        snprintf(path, sizeof(path), "/tmp/alert_%ld.jpg", time(nullptr));
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(data, 1, len, f); fclose(f); }
        fprintf(stdout, "[告警] 保存截图: %s (%zu bytes)\n", path, len);
    }
    return 0;
}

static int l_notify_send(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    fprintf(stdout, "[通知] %s\n", msg);
    return 0;
}

static const struct luaL_Reg g_lib[] = {
    {"camera_open",   l_camera_open},
    {"camera_capture", l_camera_capture},
    {"camera_close",  l_camera_close},
    {"alert_save_snapshot", l_alert_save_snapshot},
    {"notify_send",        l_notify_send},
    {nullptr, nullptr},
};

// ─── 初始化 ────────────────────────────────────────────────────────
bool lua_init() {
    g_L = luaL_newstate();
    if (!g_L) { fprintf(stderr, "[Lua] 创建状态失败\n"); return false; }
    luaL_openlibs(g_L);

    luaL_dostring(g_L, "package.path = '/usr/local/lualib/?.lua;' .. package.path");
    luaL_dostring(g_L, "package.cpath = '/usr/local/lualib/?.so;' .. package.cpath");

    lua_getglobal(g_L, "_G");
    luaL_setfuncs(g_L, g_lib, 0);
    lua_pop(g_L, 1);

    fprintf(stdout, "[Lua] 引擎初始化完成\n");
    return true;
}

void lua_close() {
    cam_close();
    if (g_L) { lua_close(g_L); g_L = nullptr; }
}

bool lua_load_script(const std::string &path) {
    std::lock_guard<std::mutex> lock(g_lua_mutex);
    if (luaL_dofile(g_L, path.c_str()) != LUA_OK) {
        fprintf(stderr, "[Lua] 加载失败 %s: %s\n", path.c_str(), lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return false;
    }
    fprintf(stdout, "[Lua] 加载脚本: %s\n", path.c_str());
    return true;
}

bool call_lua_fn(const std::string &name) {
    std::lock_guard<std::mutex> lock(g_lua_mutex);
    lua_getglobal(g_L, name.c_str());
    if (!lua_isfunction(g_L, -1)) { lua_pop(g_L, 1); return false; }
    if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "[Lua] %s 错误: %s\n", name.c_str(), lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return false;
    }
    return true;
}

void lua_call_tick() { call_lua_fn("on_tick"); }
void lua_call_on_frame(const unsigned char *, size_t, double) { call_lua_fn("on_frame"); }

void lua_call_on_ai_response(const std::string &text) {
    std::lock_guard<std::mutex> lock(g_lua_mutex);
    lua_getglobal(g_L, "on_ai_response");
    if (!lua_isfunction(g_L, -1)) { lua_pop(g_L, 1); return; }
    lua_pushstring(g_L, text.c_str());
    if (lua_pcall(g_L, 1, 0, 0) != LUA_OK) {
        fprintf(stderr, "[Lua] on_ai_response 错误: %s\n", lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
    }
}

void lua_call_on_connect(int client_fd) {
    std::thread([fd = client_fd]() {
        lua_State *L = luaL_newstate();
        luaL_openlibs(L);
        luaL_dostring(L, "package.path = '/usr/local/lualib/?.lua;' .. package.path");
        luaL_dostring(L, "package.cpath = '/usr/local/lualib/?.so;' .. package.cpath");

        lua_getglobal(L, "_G");
        luaL_setfuncs(L, g_lib, 0);
        lua_pop(L, 1);

        if (luaL_dofile(L, "scripts/ws_server.lua") != LUA_OK) {
            fprintf(stderr, "[Lua] ws_server 加载失败: %s\n", lua_tostring(L, -1));
            lua_close(L);
            return;
        }

        lua_getglobal(L, "handle_client");
        lua_pushinteger(L, fd);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            fprintf(stderr, "[Lua] handle_client 错误: %s\n", lua_tostring(L, -1));
        }
        lua_close(L);
    }).detach();
}
