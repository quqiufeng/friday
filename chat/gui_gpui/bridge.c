// bridge.c - 简单同步 HTTP 客户端 (libcurl easy interface)
// 编译: gcc -shared -fPIC -o libbridge.so bridge.c -lcurl

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#define MAX_RESP (1024*1024)

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    char **buf = (char **)userdata;
    size_t total = size * nmemb;
    if (*buf) {
        strncat(*buf, ptr, total);
        (*buf)[strlen(*buf)] = 0;  // ensure null-terminated
    }
    return total;
}

int bridge_init() {
    curl_global_init(CURL_GLOBAL_ALL);
    return 0;
}

void bridge_cleanup() {
    curl_global_cleanup();
}

// 同步 HTTP POST
int bridge_post(const char *url, const char *json_body, char *out_buf, int out_size, int timeout_sec) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char *response = calloc(1, MAX_RESP);
    char *body_copy = strdup(json_body);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_copy);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // 强制 HTTP/1.1 (HTTP/2 可能阻塞)
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    free(body_copy);

    int ret = 0;
    if (res == CURLE_OK) {
        int len = strlen(response);
        if (len > out_size - 1) len = out_size - 1;
        memcpy(out_buf, response, len);
        out_buf[len] = 0;
        ret = len;
    } else {
        fprintf(stderr, "[bridge] curl error: %s\n", curl_easy_strerror(res));
        ret = -1;
    }

    free(response);
    return ret;
}
