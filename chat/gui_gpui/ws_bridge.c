// ws_bridge.c - WebSocket 客户端连 llama-server
// 编译: gcc -shared -fPIC -o libws_bridge.so ws_bridge.c -lwebsockets -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <lws.h>

#define LLAMA_URL "http://127.0.0.1:19080"
#define BUF_SIZE 65536

static struct lws_context *ws_ctx = NULL;
static struct lws *ws_client = NULL;
static pthread_t ws_thread;
static int ws_connected = 0;
static char response_buf[BUF_SIZE];
static int response_len = 0;
static pthread_mutex_t resp_lock = PTHREAD_MUTEX_INITIALIZER;

// 回调
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        ws_connected = 1;
        fprintf(stderr, "[ws] connected\n");
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        pthread_mutex_lock(&resp_lock);
        memcpy(response_buf + response_buf, in, len);
        response_len += len;
        response_buf[response_len] = 0;
        pthread_mutex_unlock(&resp_lock);
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        ws_connected = 0;
        break;
    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    {"ws-bridge", callback_ws, 0, BUF_SIZE,},
    {NULL, NULL, 0, 0}
};

static void *ws_loop(void *arg) {
    struct lws_context_creation_info info = {0};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    ws_ctx = lws_context_create(&info);
    if (!ws_ctx) return NULL;

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = ws_ctx;
    ccinfo.address = "127.0.0.1";
    ccinfo.port = 19080;
    ccinfo.path = "/v1/realtime?mode=video";
    ccinfo.host = "127.0.0.1";
    ccinfo.origin = "127.0.0.1";
    ccinfo.protocol = protocols[0].name;

    ws_client = lws_client_connect_via_info(&ccinfo);

    while (ws_ctx) {
        lws_service(ws_ctx, 100);
    }
    return NULL;
}

// FFI 导出
int ws_bridge_init() {
    return pthread_create(&ws_thread, NULL, ws_loop, NULL);
}

int ws_bridge_send(const char *data, int len) {
    if (!ws_client || !ws_connected) return -1;
    unsigned char buf[LWS_PRE + len];
    memcpy(buf + LWS_PRE, data, len);
    return lws_write(ws_client, buf + LWS_PRE, len, LWS_WRITE_TEXT);
}

int ws_bridge_recv(char *out, int max_len) {
    pthread_mutex_lock(&resp_lock);
    int len = response_len > max_len ? max_len : response_len;
    memcpy(out, response_buf, len);
    response_len = 0;
    pthread_mutex_unlock(&resp_lock);
    return len;
}

int ws_bridge_status() { return ws_connected; }
