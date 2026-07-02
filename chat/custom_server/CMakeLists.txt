cmake_minimum_required(VERSION 3.20)
project(custom_server C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
pkg_check_modules(OpenCV REQUIRED opencv4)
pkg_check_modules(ALSA REQUIRED alsa)

include_directories(
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
    /opt/llama.cpp-omni
    /opt/llama.cpp-omni/common
    /opt/llama.cpp-omni/include
    /opt/llama.cpp-omni/ggml/include
    /opt/llama.cpp-omni/tools/omni
    /usr/local/include/luajit-2.1
)
link_directories(/opt/llama.cpp-omni/build/bin /usr/local/lib)

add_executable(gui gui.cpp)
target_link_libraries(gui PRIVATE
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${OpenCV_LIBRARIES}
    ${ALSA_LIBRARIES}
    omni llama ggml ggml-base ggml-cpu ggml-cuda
    pthread dl m cuda
    -Wl,--push-state,--no-as-needed luajit-5.1 -Wl,--pop-state
)
