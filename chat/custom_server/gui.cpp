// gui.cpp - MiniCPM-o 全双工视频语音系统 (直接链接 libomni)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <atomic>
#include <algorithm>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <opencv2/opencv.hpp>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "omni.h"
#include "common.h"

// ─── 全局状态 ──────────────────────────────────────────────────────
static std::mutex g_mtx;
static cv::Mat g_frame;
static std::atomic<bool> g_run{true};
static struct omni_context *g_ctx = nullptr;
static std::atomic<time_t> g_wake_time{0};

// AI 回复文字队列
static std::deque<std::string> g_ai_texts;
static std::string g_status = "初始化中...";

// TTS 播放设备
static const char *TTS_DEVICE = "plughw:0,3";

#define SYS(cmd) do { if (system(cmd) != 0) {} } while(0)

// ─── 唤醒词检查 ────────────────────────────────────────────────
static void wake_check() {
    FILE *f = fopen("/tmp/wake_flag", "r");
    if (!f) return;
    char buf[32];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    remove("/tmp/wake_flag");
    time_t t = atol(buf);
    if (t > g_wake_time.load()) {
        g_wake_time = t;
        printf("[唤醒] 你好 星期五！\n");
        SYS("ffplay -nodisp -autoexit -loglevel quiet /opt/friday/chat/wake_confirm.wav &");
    }
}

// ─── ALSA 录音 (直接 PCM，零子进程) ───────────────────────────
static bool record_mic(const char *wav_path) {
    static const char *devices[] = {"plughw:2,0", "hw:1,0", "plughw:3,0", "default"};
    snd_pcm_t *handle = nullptr;
    for (auto d : devices) {
        if (snd_pcm_open(&handle, d, SND_PCM_STREAM_CAPTURE, 0) == 0) break;
    }

    short buf[16000];
    int frames = 16000;
    if (handle) {
        snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 500000);
        if (snd_pcm_readi(handle, buf, frames) < 0) {
            memset(buf, 0, sizeof(buf));
        }
        snd_pcm_close(handle);
    } else {
        memset(buf, 0, sizeof(buf));
    }

    // 写 WAV 头 + PCM 数据
    auto le32 = [](int v) { return std::string{char(v), char(v>>8), char(v>>16), char(v>>24)}; };
    auto le16 = [](int v) { return std::string{char(v), char(v>>8)}; };
    int dsz = frames * 2;
    FILE *fp = fopen(wav_path, "wb");
    if (fp) {
        std::string h = "RIFF" + le32(36+dsz) + "WAVE"
                      + "fmt " + le32(16) + le16(1) + le16(1)
                      + le32(16000) + le32(32000) + le16(2) + le16(16)
                      + "data" + le32(dsz);
        fwrite(h.data(), 1, h.size(), fp);
        fwrite(buf, 2, frames, fp);
        fclose(fp);
    }
    return handle != nullptr;
}

// ─── 合并并播放 TTS ─────────────────────────────────────────────
// ─── 播放新生成的 TTS WAV ─────────────────────────────────────
static void play_tts() {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "LAST=$(cat /tmp/tts_last 2>/dev/null || echo -1); "
        "find /tmp/omni_out2 -name 'wav_*.wav' -type f 2>/dev/null "
        "| sort -t_ -k2 -n | while read F; do "
        "N=$(echo \"$F\" | sed 's/.*wav_//;s/\\.wav//'); "
        "[ \"$N\" -gt \"$LAST\" ] 2>/dev/null || continue; "
        "aplay -D %s -q \"$F\" </dev/null >/dev/null 2>&1; "
        "echo \"$N\" > /tmp/tts_last; "
        "done",
        TTS_DEVICE);
    SYS(cmd);
}

// ─── AI 工作线程 ───────────────────────────────────────────────
static void ai_worker() {
    // 设置音频增益
    SYS("amixer -c 1 sset 'Front Mic Boost' 3 2>/dev/null");
    SYS("amixer -c 1 sset 'Capture' 46 2>/dev/null");

    // 模型初始化
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_status = "加载模型中...";
    }

    common_params p{};
    p.model.path = "/data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf";
    p.vpm_model = "/data/models/MiniCPM-o-4_5-gguf/vision/MiniCPM-o-4_5-vision-F16.gguf";
    p.apm_model = "/data/models/MiniCPM-o-4_5-gguf/audio/MiniCPM-o-4_5-audio-F16.gguf";
    p.tts_model = "/data/models/MiniCPM-o-4_5-gguf/tts/MiniCPM-o-4_5-tts-F16.gguf";
    p.n_ctx = 8192;
    p.n_gpu_layers = 99;
    p.n_batch = 2048;
    p.n_ubatch = 512;
    p.use_mlock = false;
    p.sampling.temp = 0.7;
    p.n_predict = 512;

    printf("[模型] omni_init ...\n");
    g_ctx = omni_init(&p, 2, true, "/data/models/MiniCPM-o-4_5-gguf/tts",
                      100, "gpu:0", true, nullptr, nullptr, "/tmp/omni_out2");
    if (!g_ctx) {
        printf("[错误] omni_init 失败\n");
        std::lock_guard<std::mutex> lk(g_mtx);
        g_status = "模型加载失败";
        return;
    }
    g_ctx->async = true;
    g_ctx->force_listen_count = 0;
    g_ctx->listen_prob_scale = -100.0;
    g_ctx->audio_voice_clone_prompt = "<|im_start|>system\nStreaming Omni Conversation.\n<|audio_start|>";
    g_ctx->audio_assistant_prompt   = "<|audio_end|><|im_end|>\n";
    g_ctx->omni_voice_clone_prompt  = "<|im_start|>system\nStreaming Omni Conversation.\n<|audio_start|>";
    g_ctx->omni_assistant_prompt    = "<|audio_end|><|im_end|>\n";
    printf("[模型] 就绪\n");

    // 系统 prompt 初始化 (用短静音代替参考音频)
    printf("[模型] 系统 prompt 初始化\n");
    {
        // 写一个短的静音 WAV (0.1s) 用于初始化
        auto le32 = [](int v) { return std::string{char(v), char(v>>8), char(v>>16), char(v>>24)}; };
        auto le16 = [](int v) { return std::string{char(v), char(v>>8)}; };
        int dsz = 3200;  // 0.1s at 16kHz, 16-bit
        std::string h = "RIFF" + le32(36+dsz) + "WAVE"
                      + "fmt " + le32(16) + le16(1) + le16(1)
                      + le32(16000) + le32(32000) + le16(2) + le16(16)
                      + "data" + le32(dsz) + std::string(dsz, '\0');
        FILE *fp = fopen("/tmp/_silence.wav", "wb");
        if (fp) { fwrite(h.data(), 1, h.size(), fp); fclose(fp); }
        stream_prefill(g_ctx, "/tmp/_silence.wav", "", 0);
        remove("/tmp/_silence.wav");
    }

    // 摄像头初始化
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        printf("[错误] 摄像头\n");
        std::lock_guard<std::mutex> lk(g_mtx);
        g_status = "摄像头不可用";
        return;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    cv::Mat first;
    cap >> first;
    if (first.empty()) {
        printf("[错误] 无法获取首帧\n");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_frame = first.clone();
        g_status = "运行中";
    }
    printf("[摄像头] 就绪 %dx%d\n", first.cols, first.rows);

    // 摄像头采集线程
    std::thread cam_th([&cap]() {
        while (g_run) {
            cv::Mat f;
            cap >> f;
            if (!f.empty()) {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_frame = f.clone();
            }
            usleep(33000);
        }
    });

    // 主推理循环 (每 10 秒推理一次)
    int idx = 0;
    time_t last_speak = 0;
    while (g_run) {
        wake_check();
        idx++;
        char img[64], wav[64];
        snprintf(img, sizeof(img), "/tmp/f_%d.jpg", idx);
        snprintf(wav, sizeof(wav), "/tmp/m_%d.wav", idx);

        // 帧编码 (imencode 内存编码 → 写 tmpfs)
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_frame.empty()) {
                std::vector<uchar> jpg;
                cv::imencode(".jpg", g_frame, jpg, {cv::IMWRITE_JPEG_QUALITY, 70});
                FILE *fp = fopen(img, "wb");
                if (fp) { fwrite(jpg.data(), 1, jpg.size(), fp); fclose(fp); }
            }
        }

        // 录音
        record_mic(wav);

        // 冷却
        time_t now = time(nullptr);
        if (now - last_speak < 10) {
            play_tts();
            std::lock_guard<std::mutex> lk(g_mtx);
            g_status = "运行中";
            remove(img); remove(wav);
            continue;
        }
        last_speak = now;

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_status = "推理中...";
        }

        stream_prefill(g_ctx, wav, img, idx, 1);
        stream_decode(g_ctx, "/tmp/omni_out2", idx);

        // 等待 AI 文字回复 (用条件变量等待，和 server.cpp 一致)
        {
            std::unique_lock<std::mutex> lk(g_ctx->text_mtx);
            g_ctx->text_cv.wait_for(lk, std::chrono::seconds(10), [&]() {
                return !g_ctx->text_queue.empty() || g_ctx->text_done_flag;
            });
            while (!g_ctx->text_queue.empty()) {
                auto txt = g_ctx->text_queue.front();
                g_ctx->text_queue.pop_front();
                if (!txt.empty() && txt != "__IS_LISTEN__" && txt != "__END_OF_TURN__") {
                    printf("[AI] %s\n", txt.c_str());
                    std::lock_guard<std::mutex> lk2(g_mtx);
                    g_ai_texts.push_back(txt);
                    if (g_ai_texts.size() > 5) g_ai_texts.pop_front();
                }
            }
        }

        // 播放 TTS
        play_tts();

        // 恢复状态
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_status = "运行中";
        }

        remove(img);
        remove(wav);
    }

    // 清理
    if (cam_th.joinable()) cam_th.join();
    if (cap.isOpened()) cap.release();
    if (g_ctx) { /* omni_free crash bug, skip */ }
}

// ─── SDL 文字渲染辅助 (返回下一行 Y) ────────────────────────────
static int render_text(SDL_Renderer *ren, TTF_Font *font, const std::string &text,
                       int x, int y, SDL_Color color, int max_width) {
    if (text.empty() || !font) return y;

    // 自动换行
    std::string line;
    int yy = y;
    for (size_t i = 0; i < text.size(); ) {
        // 处理 UTF-8 字符
        int len = 1;
        unsigned char c = text[i];
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;

        std::string ch = text.substr(i, len);
        i += len;

        if (ch == "\n") {
            if (!line.empty()) {
                auto surf = TTF_RenderUTF8_Blended(font, line.c_str(), color);
                if (surf) {
                    auto tex = SDL_CreateTextureFromSurface(ren, surf);
                    SDL_Rect dst = {x, yy, surf->w, surf->h};
                    SDL_RenderCopy(ren, tex, nullptr, &dst);
                    SDL_DestroyTexture(tex);
                    SDL_FreeSurface(surf);
                    yy += surf->h + 2;
                }
                line.clear();
            }
            continue;
        }

        line += ch;
        int w;
        TTF_SizeUTF8(font, line.c_str(), &w, nullptr);
        if (w > max_width || i >= text.size()) {
            auto surf = TTF_RenderUTF8_Blended(font, line.c_str(), color);
            if (surf) {
                auto tex = SDL_CreateTextureFromSurface(ren, surf);
                SDL_Rect dst = {x, yy, surf->w, surf->h};
                SDL_RenderCopy(ren, tex, nullptr, &dst);
                SDL_DestroyTexture(tex);
                SDL_FreeSurface(surf);
                yy += surf->h + 2;
            }
            line.clear();
        }
    }
    return yy;
}

// ─── 主界面 ─────────────────────────────────────────────────────
int main(int, char **) {
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[错误] SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "[错误] TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    int W = 960, H = 640;
    SDL_Window *win = SDL_CreateWindow("Friday - MiniCPM-o",
                                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        W, H, SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "[错误] SDL_CreateWindow: %s\n", SDL_GetError());
        TTF_Quit(); SDL_Quit();
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        fprintf(stderr, "[错误] SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit();
        return 1;
    }

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_BGR24, SDL_TEXTUREACCESS_STREAMING, W, H);
    if (!tex) {
        fprintf(stderr, "[错误] SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit();
        return 1;
    }

    // 加载字体
    TTF_Font *font = TTF_OpenFont("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 22);
    if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", 22);
    if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 22);

    TTF_Font *font_small = TTF_OpenFont("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 16);

    std::thread ai(ai_worker);

    SDL_Event ev;
    bool quit = false;
    while (!quit) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) quit = true;
        }

        SDL_RenderClear(ren);

        // 显示摄像头画面
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_frame.empty()) {
                int fw = g_frame.cols, fh = g_frame.rows;
                float sc = std::min((float)W / fw, (float)H / fh);
                int dw = int(fw * sc), dh = int(fh * sc);
                cv::Mat r;
                cv::resize(g_frame, r, cv::Size(dw, dh));
                cv::Mat can = cv::Mat::zeros(H, W, r.type());
                r.copyTo(can(cv::Rect((W - dw) / 2, (H - dh) / 2, dw, dh)));
                SDL_UpdateTexture(tex, nullptr, can.data, can.step);
                SDL_RenderCopy(ren, tex, nullptr, nullptr);
            }
        }

        // 底部信息栏
        int BH = 110;
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
        SDL_Rect panel = {0, H - BH, W, BH};
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_RenderFillRect(ren, &panel);

        if (font) {
            SDL_Color white = {255, 255, 255, 255};
            SDL_Color green = {100, 255, 100, 255};
            SDL_Color yellow = {255, 220, 80, 255};

            SDL_Color status_color = green;
            std::string status_text;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                status_text = g_status;
                if (status_text.find("失败") != std::string::npos) status_color = {255, 80, 80, 255};
                else if (status_text.find("加载") != std::string::npos || status_text.find("推理") != std::string::npos)
                    status_color = yellow;
            }
            render_text(ren, font_small, "Friday AI · " + status_text, 10, H - BH + 8, status_color, W - 120);

            SDL_Color gray = {120, 120, 120, 255};
            render_text(ren, font_small, "ESC 退出", W - 100, H - BH + 8, gray, 100);

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (!g_ai_texts.empty()) {
                    auto texts = g_ai_texts;
                    int y = H - BH + 35;
                    for (auto it = texts.rbegin(); it != texts.rend() && y < H - 8; ++it) {
                        y = render_text(ren, font_small, *it, 10, y, white, W - 20) + 4;
                    }
                }
            }
        }

        SDL_RenderPresent(ren);
        SDL_Delay(33);
    }

    g_run = false;
    if (ai.joinable()) ai.join();

    if (font) TTF_CloseFont(font);
    if (font_small) TTF_CloseFont(font_small);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
