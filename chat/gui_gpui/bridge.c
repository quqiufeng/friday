// bridge.c - libcurl multi interface 异步 HTTP 客户端
// 编译: gcc -shared -fPIC -o libbridge.so bridge.c -lcurl -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <pthread.h>

#define MAX_RESP_SIZE 1048576

static int g_sse_detect = 0;

struct response {
    char data[MAX_RESP_SIZE];
    int len;
    int done;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    struct response *resp = (struct response *)userdata;
    size_t total = size * nmemb;
    pthread_mutex_lock(&resp->lock);
    if (resp->len + total < MAX_RESP_SIZE - 1) {
        memcpy(resp->data + resp->len, ptr, total);
        resp->len += total;
        resp->data[resp->len] = 0;
        // SSE 模式: 检测结束标记提前返回
        if (g_sse_detect && (strstr(resp->data, "[DONE]") || strstr(resp->data, "__END_OF_TURN__") || strstr(resp->data, "__IS_LISTEN__"))) {
            resp->done = 1;
            pthread_mutex_unlock(&resp->lock);
            return 0; // 中断 curl
        }
    }
    pthread_mutex_unlock(&resp->lock);
    return total;
}

// HTTP 请求
// sse_detect: 0=正常等完成, 1=检测SSE结束标记([DONE]/__IS_LISTEN__)提前返回
int bridge_post(const char *url, const char *json_body, char *out_buf, int out_size, int timeout_sec, int sse_detect) {
    static int initialized = 0;
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_ALL);
        initialized = 1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[bridge] curl_easy_init failed\n");
        return -1;
    }

    struct response resp;
    memset(&resp, 0, sizeof(resp));
    pthread_mutex_init(&resp.lock, NULL);
    pthread_cond_init(&resp.cond, NULL);

    g_sse_detect = sse_detect;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        fprintf(stderr, "[bridge] curl error: %s\n", curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);

    int ret = 0;
    if (res == CURLE_OK || res == CURLE_WRITE_ERROR) {
        pthread_mutex_lock(&resp.lock);
        int len = resp.len < out_size - 1 ? resp.len : out_size - 1;
        memcpy(out_buf, resp.data, len);
        out_buf[len] = 0;
        ret = len;
        pthread_mutex_unlock(&resp.lock);
    } else {
        ret = -1;
    }

    pthread_mutex_destroy(&resp.lock);
    pthread_cond_destroy(&resp.cond);
    return ret;
}

// 初始化
int bridge_init() {
    return 0;
}

void bridge_cleanup() {
    curl_global_cleanup();
}
