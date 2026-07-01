// gui3.cpp - HTTP 调 llama-server，TTS 正常工作
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <unistd.h>
#include <curl/curl.h>
#include <opencv2/opencv.hpp>
#include <SDL2/SDL.h>

#define LLAMA_HOST "http://127.0.0.1:19080"
#define MODEL_PATH "/data/models/MiniCPM-o-4_5-gguf/MiniCPM-o-4_5-Q4_K_M.gguf"
#define MODEL_DIR "/data/models/MiniCPM-o-4_5-gguf"

static std::mutex g_mtx;
static cv::Mat g_frame;
static std::atomic<bool> g_run{true};

static size_t wcb(void*p,size_t s,size_t n,void*d){((std::string*)d)->append((char*)p,s*n);return s*n;}
static std::string http(const std::string&url,const std::string&body="",long t=30){
    CURL*c=curl_easy_init();if(!c)return"";std::string r;
    curl_easy_setopt(c,CURLOPT_URL,url.c_str());
    if(!body.empty())curl_easy_setopt(c,CURLOPT_POSTFIELDS,body.c_str());
    curl_easy_setopt(c,CURLOPT_TIMEOUT,t);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,wcb);
    curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
    struct curl_slist*h=nullptr;h=curl_slist_append(h,"Content-Type: application/json");
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);
    curl_easy_perform(c);curl_slist_free_all(h);curl_easy_cleanup(c);
    return r;
}

static void ai_worker(){
    // 启动 llama-server
    system("pkill -9 llama-server 2>/dev/null; sleep 1");
    char cmd[512];snprintf(cmd,sizeof(cmd),
        "/opt/llama.cpp-omni/build/bin/llama-server --host 127.0.0.1 --port 19080"
        " --model '%s' --ctx-size 8192 --n-gpu-layers 99"
        " --repeat-penalty 1.05 --temp 0.7 > /tmp/llama-server.log 2>&1 &",MODEL_PATH);
    system(cmd);
    
    for(int i=0;;i++){
        auto r=http(LLAMA_HOST"/health","",2);
        if(r.find("\"status\":\"ok\"")!=std::string::npos){printf("[模型] %ds\n",i+1);break;}
        printf(".");fflush(stdout);usleep(1000000);
    }

    // omni_init
    auto r=http(LLAMA_HOST"/v1/stream/omni_init",
        "{\"media_type\":2,\"use_tts\":true,\"duplex_mode\":true,"
        "\"model_dir\":\"" MODEL_DIR "\",\"tts_bin_dir\":\"" MODEL_DIR "/tts\","
        "\"tts_gpu_layers\":100,\"token2wav_device\":\"gpu:0\",\"output_dir\":\"/tmp/omni_out\","
        "\"voice_clone_prompt\":\"<|im_start|>system\\n每次收到画面都必须用中文说话。描述画面内容。\\n<|audio_start|>\","
        "\"assistant_prompt\":\"<|audio_end|><|im_end|>\\n\",\"n_predict\":512}",300);
    printf("[模型] %s\n",r.empty()?"失败":"就绪");

    // 摄像头
    cv::VideoCapture cap(0);
    if(!cap.isOpened()){printf("[错误] 摄像头\n");return;}
    cap.set(cv::CAP_PROP_FRAME_WIDTH,640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT,480);
    printf("[摄像头] 就绪\n");

    int idx=0;
    while(g_run){
        idx++;
        cv::Mat f;cap>>f;
        if(f.empty()){usleep(500000);continue;}

        // 共享最新帧给 GUI
        {std::lock_guard<std::mutex>lk(g_mtx);g_frame=f.clone();}

        // 存 JPEG
        char img[64],wav[64];
        snprintf(img,sizeof(img),"/tmp/f_%d.jpg",idx);
        snprintf(wav,sizeof(wav),"/tmp/m_%d.wav",idx);
        cv::imwrite(img,f);

        // 录音
        bool mic_ok=false;
        for(auto d:{"hw:1,0","hw:3,0","hw:2,0","default"}){
            char rc[256];snprintf(rc,sizeof(rc),"ffmpeg -f alsa -ac 1 -ar 16000 -i %s -t 1 -y %s 2>/dev/null",d,wav);
            if(system(rc)==0){mic_ok=true;break;}
        }
        if(!mic_ok){
            auto L32=[](int v){return std::string{char(v),char(v>>8),char(v>>16),char(v>>24)};};
            auto L16=[](int v){return std::string{char(v),char(v>>8)};};
            std::string w="RIFF"+L32(36+64000)+"WAVE"+"fmt "+L32(16)+L16(3)+L16(1)+L32(16000)+L32(64000)+L16(4)+L16(32)+"data"+L32(64000)+std::string(64000,0);
            FILE*fp=fopen(wav,"wb");fwrite(w.data(),1,w.size(),fp);fclose(fp);
        }

        // prefill
        char js[256];snprintf(js,sizeof(js),"{\"audio_path_prefix\":\"%s\",\"img_path_prefix\":\"%s\",\"cnt\":%d}",wav,img,idx);
        http(LLAMA_HOST"/v1/stream/prefill",js,10);

        // decode (SSE, 短超时)
        auto resp=http(LLAMA_HOST"/v1/stream/decode","{\"debug_dir\":\"/tmp/omni_out\",\"stream\":true}",30);

        // 解析 SSE 找文字
        size_t p=0;
        while(p<resp.size()){
            auto d=resp.find("data: ",p);if(d==-1)break;d+=6;
            auto e=resp.find("\n",d);if(e==-1)e=resp.size();
            auto l=resp.substr(d,e-d);
            if(l=="[DONE]")break;
            auto j=l.find("\"content\":\"");if(j!=-1){
                j+=11;auto k=l.find("\"",j);if(k!=-1){
                    auto t=l.substr(j,k-j);
                    if(!t.empty()&&t!="__IS_LISTEN__"&&t!="__END_OF_TURN__"){
                        // 写到文件用 espeak 播放
                        FILE*fp=fopen("/tmp/speak.txt","w");
                        if(fp){fwrite(t.data(),1,t.size(),fp);fclose(fp);}
                        system("espeak-ng -v zh -f /tmp/speak.txt &");
                    }
                }
            }
            p=e+1;
        }

        // 播放 TTS 音频（如生成）
        system("ls -t /tmp/omni_out/round_*/tts_wav/wav_*.wav 2>/dev/null|head -1|xargs -r aplay -q &");

        remove(img);remove(wav);
        usleep(500000);
    }
}

int main(int,char**){
    setbuf(stdout,nullptr);
    SDL_Init(SDL_INIT_VIDEO);
    int w=960,h=640;
    auto win=SDL_CreateWindow("MiniCPM-o",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,w,h,SDL_WINDOW_RESIZABLE);
    auto ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED);
    auto tex=SDL_CreateTexture(ren,SDL_PIXELFORMAT_BGR24,SDL_TEXTUREACCESS_STREAMING,w,h);
    std::thread ai(ai_worker);ai.detach();
    SDL_Event ev;bool quit=false;
    while(!quit){
        while(SDL_PollEvent(&ev)){if(ev.type==SDL_QUIT)quit=true;}
        SDL_RenderClear(ren);
        {std::lock_guard<std::mutex>lk(g_mtx);
            if(!g_frame.empty()){
                int fw=g_frame.cols,fh=g_frame.rows;
                float sc=std::min((float)w/fw,(float)h/fh);
                int dw=int(fw*sc),dh=int(fh*sc);
                cv::Mat r;cv::resize(g_frame,r,cv::Size(dw,dh));
                cv::Mat can=cv::Mat::zeros(h,w,r.type());
                r.copyTo(can(cv::Rect((w-dw)/2,(h-dh)/2,dw,dh)));
                SDL_UpdateTexture(tex,nullptr,can.data,can.step);
                SDL_RenderCopy(ren,tex,nullptr,nullptr);
            }
        }
        SDL_RenderPresent(ren);SDL_Delay(33);
    }
    g_run=false;usleep(500000);
    system("pkill -9 llama-server 2>/dev/null");
    SDL_DestroyTexture(tex);SDL_DestroyRenderer(ren);SDL_DestroyWindow(win);
    SDL_Quit();return 0;
}
