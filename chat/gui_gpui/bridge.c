// bridge.c - HTTP 客户端桥接 llama-server omni API
// 维护 duplex session 状态
// 编译: gcc -shared -fPIC -o libbridge.so bridge.c -lcurl -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <pthread.h>

static CURL *curl = NULL;
static int cnt = 0;
static int is_listening = 1;
static char resp_buf[1048576];
static int resp_len = 0;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    if (resp_len + total < sizeof(resp_buf)) {
        memcpy(resp_buf + resp_len, ptr, total);
        resp_len += total;
        resp_buf[resp_len] = 0;
    }
    return total;
}

// 初始化 bridge (启动 llama-server + omni_init)
int bridge_init() {
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (!curl) return -1;

    // 等待 llama-server 就绪 (不自己启动，由外部管理)
    for (int i = 0; i < 180; i++) {
        curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19080/health");
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        if (curl_easy_perform(curl) == CURLE_OK) break;
        sleep(5);
    }

    // omni_init 由外部脚本调用，这里只初始化状态
    is_listening = 1;
    cnt = 0;
    return 0;
}

// 发送一帧 (图片路径 + 音频路径)
// 返回: 0=listen, 1=speak (有文字), -1=错误
// 文字输出到 out_text (max_len)
int bridge_process_frame(const char *img_path, const char *wav_path, char *out_text, int max_len) {
    if (!curl) return -1;

    cnt++;
    resp_len = 0;

    if (is_listening) {
        // listen 状态：只发 prefill (不带图片)，然后 decode
        // 实际上 duplex 模式的 listen 只需要 decode
        curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19080/v1/stream/decode");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);

        char json[256];
        snprintf(json, sizeof(json), "{\"stream\":true}");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
        struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        resp_len = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (res == CURLE_OK) {
            // 解析 SSE
            char *line = strtok(resp_buf, "\n");
            while (line) {
                if (strncmp(line, "data: ", 6) == 0) {
                    char *data_str = line + 6;
                    if (strcmp(data_str, "[DONE]") == 0) break;
                    // 简单检查 is_listen
                    if (strstr(data_str, "\"is_listen\":true") || strstr(data_str, "__IS_LISTEN__")) {
                        // 继续 listen
                        break;
                    }
                    // 提取 content
                    char *content = strstr(data_str, "\"content\":\"");
                    if (content) {
                        content += 11;
                        char *end = strchr(content, '"');
                        if (end) {
                            int len = end - content;
                            if (len > max_len - 1) len = max_len - 1;
                            memcpy(out_text, content, len);
                            out_text[len] = 0;
                            is_listening = 0;
                            return 1;
                        }
                    }
                }
                line = strtok(NULL, "\n");
            }
        }
        return 0;
    } else {
        // 说话状态：发 prefill (带图片) + decode
        curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19080/v1/stream/prefill");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);

        char json[512];
        snprintf(json, sizeof(json),
            "{\"audio_path_prefix\":\"%s\",\"img_path_prefix\":\"%s\",\"cnt\":%d}",
            wav_path, img_path, cnt);

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
        struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        resp_len = 0;
        curl_easy_perform(curl);
        curl_slist_free_all(headers);

        // decode
        curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19080/v1/stream/decode");
        snprintf(json, sizeof(json), "{\"debug_dir\":\"/tmp/omni_out2\",\"stream\":true}");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        resp_len = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);

        if (res == CURLE_OK) {
            char *line = strtok(resp_buf, "\n");
            while (line) {
                if (strncmp(line, "data: ", 6) == 0) {
                    char *data_str = line + 6;
                    if (strcmp(data_str, "[DONE]") == 0) break;
                    if (strstr(data_str, "\"is_listen\":true") || strstr(data_str, "__IS_LISTEN__")) {
                        is_listening = 1;
                        break;
                    }
                    char *content = strstr(data_str, "\"content\":\"");
                    if (content) {
                        content += 11;
                        char *end = strchr(content, '"');
                        if (end) {
                            int len = end - content;
                            if (len > max_len - 1) len = max_len - 1;
                            memcpy(out_text, content, len);
                            out_text[len] = 0;
                            return 1;
                        }
                    }
                }
                line = strtok(NULL, "\n");
            }
        }
        return 0;
    }
}

void bridge_cleanup() {
    if (curl) {
        curl_easy_cleanup(curl);
        curl = NULL;
    }
    curl_global_cleanup();
}
