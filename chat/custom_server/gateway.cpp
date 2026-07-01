#include "gateway.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

#include "lua_bridge.h"

static std::atomic<bool> g_running{false};
static int g_listen_fd = -1;
static int g_port = 8040;

static void accept_loop() {
    fprintf(stdout, "[TCP] 监听 0.0.0.0:%d (WS 协议由 Lua 处理)\n", g_port);

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(g_listen_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) {
            if (g_running) usleep(10000);
            continue;
        }

        // 保持阻塞模式（Lua FFI 需要阻塞 read/write）
        // int flags = fcntl(client_fd, F_GETFL, 0);
        // fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // 交给 Lua 处理 WebSocket 握手和协议
        lua_call_on_connect(client_fd);
    }
}

bool gateway_start(int port, void *) {
    g_port = port;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        return false;
    }

    int reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_listen_fd);
        return false;
    }

    if (listen(g_listen_fd, 128) < 0) {
        perror("listen");
        close(g_listen_fd);
        return false;
    }

    g_running = true;
    std::thread(accept_loop).detach();
    return true;
}

void gateway_stop() {
    g_running = false;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}
