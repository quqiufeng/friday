cmake_minimum_required(VERSION 3.20)
project(custom_server C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
pkg_check_modules(OpenCV REQUIRED opencv4)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(LUAJIT REQUIRED luajit)

# ─── 头文件 ────────────────────────────────────────────────────────
include_directories(
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    ${LUAJIT_INCLUDE_DIRS}
    /opt/llama.cpp-omni
    /opt/llama.cpp-omni/common
    /opt/llama.cpp-omni/include
    /opt/llama.cpp-omni/ggml/include
    /opt/llama.cpp-omni/tools/omni
)
link_directories(/opt/llama.cpp-omni/build/bin)

# ─── GUI2 主程序 (直接链接 libomni) ───────────────────────────────
add_executable(gui gui.cpp)
target_link_libraries(gui PRIVATE
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${OpenCV_LIBRARIES}
    ${ALSA_LIBRARIES}
    ${LUAJIT_LIBRARIES}
    omni llama ggml ggml-base ggml-cpu ggml-cuda
    pthread dl m cuda
)

# ─── 服务端（C++ + LuaJIT，可选） ────────────────────────────────
# add_executable(custom_server main.cpp gateway.cpp camera.cpp lua_bridge.cpp)
# target_link_libraries(custom_server PRIVATE luajit-5.1 curl pthread dl m)
