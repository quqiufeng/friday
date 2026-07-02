// gui.cpp - MiniCPM-o 全双工视频语音系统 (DuplexSession API)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <algorithm>
#include <unistd.h>
#include <dirent.h>
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
static const char *TTS_DEVICE = "plughw:3,0";

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

// ─── 生成静音 WAV (16-bit PCM, 16kHz, 1s) ─────────────────────
static void make_silence_wav(const char *path) {
    auto L32 = [](int v) -> std::string {
        return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
    };
    auto L16 = [](int v) -> std::string {
        return std::string{char(v), char(v >> 8)};
    };
    int sr = 16000;
    int samples = sr;
    int data_size = samples * 2;
    std::string w = "RIFF" + L32(36 + data_size) + "WAVE"
                  + "fmt " + L32(16) + L16(1) + L16(1)
                  + L32(sr) + L32(sr * 2) + L16(2) + L16(16)
                  + "data" + L32(data_size)
                  + std::string(data_size, '\0');
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(w.data(), 1, w.size(), fp); fclose(fp); }
}

// ─── ALSA 录音 (直接 PCM，返回是否成功) ───────────────────────
static const char *g_rtsp_url = nullptr;

static bool record_mic(const char *wav_path) {
    // USB 摄像头麦克风 (plughw:2,0) 或本地 ALSA
    static const char *devices[] = {"plughw:2,0", "default", "hw:1,0"};
    snd_pcm_t *handle = nullptr;
    for (auto d : devices) {
        if (snd_pcm_open(&handle, d, SND_PCM_STREAM_CAPTURE, 0) == 0) { printf("[alsa] 打开 %s 成功\n", d); break; }
    }
    short buf[16000];
    int frames = 16000;
    int r = -1;
    if (handle) {
        snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 500000);
        r = snd_pcm_readi(handle, buf, frames);
        if (r < 0) { printf("[alsa] 读取错误: %s\n", snd_strerror(r)); memset(buf, 0, sizeof(buf)); }
        else frames = r;
        snd_pcm_close(handle);
    } else {
        printf("[alsa] 无法打开设备\n");
        memset(buf, 0, sizeof(buf));
    }
    int peak_amp = 0; for (int i = 0; i < frames; i++) { int a = abs(buf[i]); if (a > peak_amp) peak_amp = a; }
    float gain = (peak_amp < 1000) ? 4.0f : std::min(32767.0f / std::max(peak_amp, 1), 3.0f);
    for (int i = 0; i < frames; i++) { float v = buf[i] * gain; if (v > 32767) v = 32767; if (v < -32768) v = -32768; buf[i] = (short)v; }
    auto le32 = [](int v) { return std::string{char(v), char(v>>8), char(v>>16), char(v>>24)}; };
    auto le16 = [](int v) { return std::string{char(v), char(v>>8)}; };
    int dsz = frames * 2;
    FILE *fp = fopen(wav_path, "wb");
    if (fp) {
        std::string h = "RIFF" + le32(36+dsz) + "WAVE" + "fmt " + le32(16) + le16(1) + le16(1)
                      + le32(16000) + le32(32000) + le16(2) + le16(16) + "data" + le32(dsz);
        fwrite(h.data(), 1, h.size(), fp);
        fwrite(buf, 2, frames, fp);
        fclose(fp);
    }
    return r >= 0;
}

// ─── 过滤 think 标签 ────────────────────────────────────────
static std::string strip_think(const std::string & s) {
    std::string r = s;
    for (;;) {
        auto p = r.find("<think>");   if (p != std::string::npos) { r.erase(p, 7); continue; }
        auto q = r.find("</think>");  if (q != std::string::npos) { r.erase(q, 8); continue; }
        break;
    }
    return r;
}

// ─── 直接 ALSA 播放 WAV (零子进程) ─────────────────────────
static void alsa_play(const char *wav_path) {
    FILE *fp = fopen(wav_path, "rb");
    if (!fp) { printf("[TTS] 文件不存在: %s\n", wav_path); return; }
    // 跳过 WAV 头 (44 bytes)
    char hdr[44];
    if (fread(hdr, 1, 44, fp) != 44) { fclose(fp); return; }
    int sr = *(int*)(hdr + 24);
    short channels = *(short*)(hdr + 22);
    short bits = *(short*)(hdr + 34);
    int data_bytes = *(int*)(hdr + 40);
    if (data_bytes <= 0) { data_bytes = 1024 * 1024; } // fallback
    int data_samples = data_bytes / (bits / 8);

    int alsa_fmt = (bits == 16) ? SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_S32_LE;
    snd_pcm_t *handle = nullptr;
    if (snd_pcm_open(&handle, TTS_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        printf("[TTS] 无法打开 %s\n", TTS_DEVICE); fclose(fp); return;
    }
    snd_pcm_set_params(handle, (snd_pcm_format_t)alsa_fmt, SND_PCM_ACCESS_RW_INTERLEAVED,
                       channels, sr, 1, 500000);

    const int BUF = 4096;
    short buf[BUF];
    int total = 0;
    while (total < data_samples && g_run) {
        int n = std::min(BUF, data_samples - total);
        int r = (int)fread(buf, bits / 8, n, fp);
        if (r <= 0) break;
        int written = 0;
        while (written < r) {
            int w = snd_pcm_writei(handle, buf + written, r - written);
            if (w < 0) { snd_pcm_prepare(handle); continue; }
            written += w;
        }
        total += r;
    }
    printf("[TTS] 播放完成: %s (%d samples, %d bytes)\n", wav_path, total, total * (bits / 8));
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
    fclose(fp);
}

// ─── ALSA 一次性播放多个 WAV (零子进程) ─────────────────────
static void play_tts() {
    if (!g_run) return;

    // 读 last
    FILE *f_last = fopen("/tmp/tts_last", "r");
    int last = -1;
    if (f_last) { fscanf(f_last, "%d", &last); fclose(f_last); }

    // 扫描 tts_wav 目录
    std::vector<std::string> files;
    int max_n = last;
    DIR *dir = opendir("/tmp/omni_out2/tts_wav");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        int n;
        if (sscanf(ent->d_name, "wav_%d.wav", &n) == 1) {
            if (n > last) {
                files.push_back(ent->d_name);
                if (n > max_n) max_n = n;
            }
        }
    }
    closedir(dir);
    if (files.empty()) return;
    // 按编号排序
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        int na = 0, nb = 0;
        sscanf(a.c_str(), "wav_%d.wav", &na);
        sscanf(b.c_str(), "wav_%d.wav", &nb);
        return na < nb;
    });

    printf("[TTS] 播放 %zu 个文件:", files.size());
    for (auto &f : files) printf(" %s", f.c_str());
    printf("\n");

    // 逐个 ALSA 直写
    for (auto &f : files) {
        std::string path = "/tmp/omni_out2/tts_wav/" + f;
        alsa_play(path.c_str());
        if (!g_run) break;
    }

    FILE *fw = fopen("/tmp/tts_last", "w");
    if (fw) { fprintf(fw, "%d", max_n); fclose(fw); }
}

// ─── TTS 环形缓冲 + 后台播放线程 ────────────────────────────
static const int TTS_RING_SIZE = 48000 * 10; // 10s @24kHz
static float g_tts_ring[TTS_RING_SIZE];
static std::atomic<int> g_tts_w{0}, g_tts_r{0};
static std::atomic<bool> g_tts_active{false};
static std::mutex g_tts_cv_mtx;
static std::condition_variable g_tts_cv;

static void tts_playback_thread() {
    snd_pcm_t *h = nullptr;
    if (snd_pcm_open(&h, TTS_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0) return;
    snd_pcm_set_params(h, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 24000, 1, 500000);
    // 预填充 100ms 静音防止启动爆音
    std::vector<float> silence(2400, 0.0f);
    snd_pcm_writei(h, silence.data(), 2400);

    float buf[2400];
    while (g_run) {
        int r = g_tts_r.load();
        int w = g_tts_w.load();
        int avail = (w - r + TTS_RING_SIZE) % TTS_RING_SIZE;
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        int take = std::min(avail, 2400);
        for (int i = 0; i < take; i++)
            buf[i] = g_tts_ring[(r + i) % TTS_RING_SIZE];
        g_tts_r.store((r + take) % TTS_RING_SIZE);
        int written = 0;
        while (written < take) {
            int wb = snd_pcm_writei(h, buf + written, take - written);
            if (wb == -EPIPE) { snd_pcm_prepare(h); continue; }
            if (wb < 0) break;
            written += wb;
        }
    }
    snd_pcm_drain(h);
    snd_pcm_close(h);
}

static void tts_audio_cb(const float *samples, int n_samples, int sample_rate, bool is_final) {
    if (!g_run || n_samples <= 0) return;
    g_tts_active = true;
    // 重采样到 24kHz（如果 sample_rate 不是 24kHz）
    if (sample_rate != 24000) {
        int out_len = n_samples * 24000 / sample_rate;
        std::vector<float> resampled(out_len);
        for (int i = 0; i < out_len; i++) {
            float src = (float)i * sample_rate / 24000;
            int si = (int)src; float frac = src - si;
            if (si + 1 < n_samples) resampled[i] = samples[si] * (1 - frac) + samples[si + 1] * frac;
            else resampled[i] = samples[std::min(si, n_samples - 1)];
        }
        samples = resampled.data();
        n_samples = out_len;
    }
    int w = g_tts_w.load();
    for (int i = 0; i < n_samples; i++) {
        g_tts_ring[(w + i) % TTS_RING_SIZE] = samples[i];
    }
    g_tts_w.store((w + n_samples) % TTS_RING_SIZE);
    if (is_final) { g_tts_active = false; g_tts_cv.notify_one(); }
}

// ─── 录音线程：100ms 非阻塞采集，累积 1s 推帧 ─────────────
static std::mutex g_audio_ready_mtx;
static std::condition_variable g_audio_ready_cv;
static std::atomic<bool> g_audio_ready{false};
static char g_audio_wav[64];
static char g_audio_img[64];
static short g_audio_buf[8000];
static int g_audio_count = 0;
static int g_audio_idx = 0;

static void capture_thread_func() {
    // 打开捕获设备
    snd_pcm_t *cap = nullptr;
    for (auto d : {"plughw:2,0", "default", "hw:1,0"}) {
        if (snd_pcm_open(&cap, d, SND_PCM_STREAM_CAPTURE, 0) == 0) { printf("[alsa] 捕获 %s\n", d); break; }
    }
    if (!cap) { printf("[alsa] 无法打开捕获设备\n"); return; }
    snd_pcm_set_params(cap, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 500000);

    const int CHUNK = 1600; // 100ms
    short chunk[CHUNK];
    while (g_run) {
        int r = snd_pcm_readi(cap, chunk, CHUNK);
        if (r < 0) { snd_pcm_prepare(cap); continue; }
        // 自适应增益
        int peak = 0; for (int i = 0; i < r; i++) { int a = abs(chunk[i]); if (a > peak) peak = a; }
        float gain = (peak < 1000) ? 4.0f : std::min(32767.0f / std::max(peak, 1), 3.0f);
        for (int i = 0; i < r; i++) { float v = chunk[i] * gain; if (v > 32767) v = 32767; if (v < -32768) v = -32768; chunk[i] = (short)v; }
        // 累加到环形缓冲
        int n = std::min(r, 8000 - g_audio_count);
        memcpy(g_audio_buf + g_audio_count, chunk, n * 2);
        g_audio_count += n;
        if (g_audio_count >= 8000) {
            // 写 WAV (500ms)
            g_audio_idx++;
            snprintf(g_audio_wav, sizeof(g_audio_wav), "/tmp/m_%d.wav", g_audio_idx);
            snprintf(g_audio_img, sizeof(g_audio_img), "/tmp/f_%d.jpg", g_audio_idx);
            auto le32 = [](int v) { return std::string{char(v),char(v>>8),char(v>>16),char(v>>24)}; };
            auto le16 = [](int v) { return std::string{char(v),char(v>>8)}; };
            int dsz = 8000 * 2;
            std::string h = "RIFF" + le32(36+dsz) + "WAVE" + "fmt " + le32(16) + le16(1) + le16(1)
                          + le32(16000) + le32(32000) + le16(2) + le16(16) + "data" + le32(dsz);
            FILE *fw = fopen(g_audio_wav, "wb");
            if (fw) { fwrite(h.data(), 1, h.size(), fw); fwrite(g_audio_buf, 2, 8000, fw); fclose(fw); }
            // 保存帧
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (!g_frame.empty()) cv::imwrite(g_audio_img, g_frame);
            }
            printf("[mic] peak=%d%s\n", peak, peak > 3000 ? " 🔊人声" : peak > 500 ? " 环境音" : " 静音");
            g_audio_count = 0;
            g_audio_ready = true;
            g_audio_ready_cv.notify_one();
        }
    }
    snd_pcm_close(cap);
}

// ─── AI 工作线程 (DuplexSession API + 流式 TTS) ──────────────
static void ai_worker() {
    SYS("amixer -c 2 sset 'Mic Capture Volume' 7810 2>/dev/null");
    SYS("amixer -c 2 sset 'Mic Capture Switch' on 2>/dev/null");
    SYS("amixer -c 1 sset 'Auto-Mute Mode' Disabled 2>/dev/null");
    SYS("amixer -c 3 sset 'PCM Playback Switch' on 2>/dev/null");
    SYS("amixer -c 3 sset 'PCM Playback Volume' 63 2>/dev/null");

    const char *OUTPUT_DIR = "/tmp/omni_out2";
    const char *REF_AUDIO  = "/opt/friday/chat/web/assets/ref_audio/ref_minicpm_signature.wav";

    { std::lock_guard<std::mutex> lk(g_mtx); g_status = "加载模型中..."; }

    common_params p{};
    p.model.path = "/data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf";
    p.vpm_model = "/data/models/MiniCPM-o-4_5-gguf/vision/MiniCPM-o-4_5-vision-F16.gguf";
    p.apm_model = "/data/models/MiniCPM-o-4_5-gguf/audio/MiniCPM-o-4_5-audio-F16.gguf";
    p.tts_model = "/data/models/MiniCPM-o-4_5-gguf/tts/MiniCPM-o-4_5-tts-F16.gguf";
    p.n_ctx = 8192; p.n_gpu_layers = 99; p.n_batch = 2048; p.n_ubatch = 512;
    p.use_mlock = false; p.sampling.temp = 0.7; p.sampling.top_k = 50; p.sampling.top_p = 0.9; p.n_predict = 512;

    printf("[模型] omni_init ...\n");
    g_ctx = omni_init(&p, 2, true, "/data/models/MiniCPM-o-4_5-gguf/tts", 100, "gpu:0", true, nullptr, nullptr, OUTPUT_DIR);
    if (!g_ctx) { printf("[错误] omni_init 失败\n"); g_status = "模型加载失败"; return; }
    g_ctx->async = true;
    g_ctx->force_listen_count = 1;
    g_ctx->max_new_speak_tokens_per_chunk = 200;
    g_ctx->listen_prob_scale = 1.0;
    g_ctx->length_penalty = 1.1;
    g_ctx->language = "zh";
    g_ctx->audio_output_cb = tts_audio_cb;
    std::thread tts_play_th(tts_playback_thread);
    tts_play_th.detach();
    g_ctx->omni_voice_clone_prompt = "<|im_start|>system\n模仿音频样本的音色并生成新的内容。\n<|audio_start|>";
    g_ctx->omni_assistant_prompt = "你是一个有用的语音助手，请仔细听用户的语音并回答问题，不要主动描述画面内容。全程只用中文回答，不要使用英文。请用高自然度的方式和用户聊天。";
    printf("[模型] 就绪\n");

    // 摄像头
    const char *rtsp = getenv("CAMERA_RTSP_URL"); g_rtsp_url = rtsp;
    cv::VideoCapture cap;
    if (rtsp) cap.open(rtsp, cv::CAP_FFMPEG); else cap.open(0, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("[错误] 摄像头\n"); g_status = "摄像头不可用"; return; }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640); cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480); cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cv::Mat first; cap >> first;
    if (first.empty()) { printf("[错误] 首帧\n"); return; }
    { std::lock_guard<std::mutex> lk(g_mtx); g_frame = first.clone(); g_status = "运行中"; }
    printf("[摄像头] 就绪 %dx%d\n", first.cols, first.rows);

    // 启动采集线程
    std::thread cap_th(capture_thread_func);

    // 摄像头采集线程
    std::thread cam_th([&cap]() {
        while (g_run) { cv::Mat f; cap >> f; if (!f.empty()) { std::lock_guard<std::mutex> lk(g_mtx); g_frame = f.clone(); } usleep(33000); }
    });

    // DuplexSession
    { std::lock_guard<std::mutex> lk(g_mtx); g_status = "启动会话..."; }
    SYS("rm -f /tmp/omni_out2/tts_wav/wav_*.wav /tmp/tts_last 2>/dev/null");
    if (!omni_duplex_session_begin(g_ctx, REF_AUDIO, OUTPUT_DIR)) { printf("[错误] duplex begin 失败\n"); g_status = "会话启动失败"; return; }
    printf("[Duplex] 会话已启动\n");
    { std::lock_guard<std::mutex> lk(g_mtx); g_status = "等待语音输入..."; }

    // 主循环
    bool speaking = false;
    std::string speak_buf;
    while (g_run) {
        wake_check();
        {
            std::unique_lock<std::mutex> lk(g_audio_ready_mtx);
            g_audio_ready_cv.wait_for(lk, std::chrono::milliseconds(500), []{ return g_audio_ready.load(); });
            g_audio_ready = false;
        }

        int peak = 0;
        { FILE *f = fopen(g_audio_wav, "rb"); if (f) { fseek(f, 44, SEEK_SET); short s; while (fread(&s, 2, 1, f) == 1) { int a = abs(s); if (a > peak) peak = a; } fclose(f); } }
        if (peak < 300 && !speaking && speak_buf.empty()) {
            { std::lock_guard<std::mutex> lk(g_mtx); g_status = "等待语音输入..."; }
            remove(g_audio_wav); remove(g_audio_img); continue;
        }

        { std::lock_guard<std::mutex> lk(g_mtx); g_status = speaking ? "AI 说话中..." : "推理中..."; }

        OmniDuplexFrame frame;
        frame.aud_fname = g_audio_wav; frame.img_fname = g_audio_img;
        frame.max_slice_nums = 1; frame.user_seq = g_audio_idx;
        int64_t fid = omni_duplex_push_frame(g_ctx, frame);
        if (fid < 0) break;

        OmniDuplexFrameResult result;
        if (!omni_duplex_wait_next_frame(g_ctx, &result, 10000)) { remove(g_audio_wav); remove(g_audio_img); continue; }
        if (!result.ok) { remove(g_audio_wav); remove(g_audio_img); continue; }

        if (result.is_speak) {
            speaking = true;
            speak_buf += strip_think(result.text);
            // 每帧即时显示累积文字（不等语音播完）
            printf("[AI] %s\n", speak_buf.c_str());
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_ai_texts.empty()) g_ai_texts.pop_back();
            g_ai_texts.push_back(speak_buf);
        } else if (speaking) {
            speaking = false;
            speak_buf.clear();
        }
        remove(g_audio_wav); remove(g_audio_img);
    }

    omni_duplex_session_end(g_ctx);
    if (cam_th.joinable()) cam_th.join();
    if (cap_th.joinable()) cap_th.join();
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

    int W = 1280, H = 720;
    SDL_Window *win = SDL_CreateWindow("Friday 老秋专属",
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
        int BH = 200;
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
            render_text(ren, font_small, "老秋专属 · " + status_text, 10, H - BH + 8, status_color, W - 120);

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
