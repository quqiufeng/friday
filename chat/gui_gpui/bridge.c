// bridge.c - 简单同步 HTTP 客户端 (每请求一个线程)
// 编译: gcc -shared -fPIC -o libbridge.so bridge.c -lcurl -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <pthread.h>

#define MAX_RESP (1024*1024)

struct req_state {
    char url[512];
    char body[4096];
    char resp[MAX_RESP];
    int resp_len;
    int done;
    int sse_mode;
    int timeout_sec;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    struct req_state *rs = (struct req_state *)userdata;
    size_t total = size * nmemb;
    if (rs->resp_len + total < MAX_RESP - 1) {
        memcpy(rs->resp + rs->resp_len, ptr, total);
        rs->resp_len += total;
        rs->resp[rs->resp_len] = 0;
    }
    return total;
}

static void *request_thread(void *arg) {
    struct req_state *rs = (struct req_state *)arg;
    CURL *curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_URL, rs->url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, rs->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, rs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)rs->timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        fprintf(stderr, "[bridge] %s: %s\n", rs->url, curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    rs->done = 1;
    return NULL;
}

int bridge_init() {
    curl_global_init(CURL_GLOBAL_ALL);
    return 0;
}

void bridge_cleanup() {
    curl_global_cleanup();
}

// 同步 POST (阻塞直到完成)
int bridge_post(const char *url, const char *json_body, char *out_buf, int out_size, int timeout_sec, int sse_mode) {
    struct req_state rs;
    memset(&rs, 0, sizeof(rs));
    strncpy(rs.url, url, sizeof(rs.url) - 1);
    strncpy(rs.body, json_body, sizeof(rs.body) - 1);
    rs.sse_mode = sse_mode;
    rs.timeout_sec = timeout_sec;

    pthread_t tid;
    pthread_create(&tid, NULL, request_thread, &rs);
    pthread_join(tid, NULL); // 阻塞等待

    int len = rs.resp_len < out_size - 1 ? rs.resp_len : out_size - 1;
    memcpy(out_buf, rs.resp, len);
    out_buf[len] = 0;
    return len;
}
