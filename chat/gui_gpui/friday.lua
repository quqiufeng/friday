package.cpath = "/opt/friday/chat/gui_gpui/?.so;/usr/local/lualib/?.so;" .. package.cpath
local ffi = require("ffi")
local cjson = require("cjson")

ffi.cdef[[
    int bridge_init();
    void bridge_cleanup();
    int bridge_post(const char *url, const char *json, char *out, int out_size, int timeout);
]]

local bridge = ffi.load("/opt/friday/chat/gui_gpui/libbridge.so")
local llama_url = "http://127.0.0.1:19080"
local out = ffi.new("char[?]", 1048576)

bridge.bridge_init()

local function http_post(path, body, timeout)
    return bridge.bridge_post(llama_url .. path, body, out, 1048576, timeout or 30)
end

local function extract_text(len)
    local results = {}
    if len <= 0 then return results end
    local s = ffi.string(out, len)
    for line in s:gmatch("[^\r\n]+") do
        if line:find("^data: ") then
            local d = line:sub(7)
            if d ~= "[DONE]" then
                local c = d:match('"content"%s*:%s*"([^"]*)"')
                if c and c ~= "" and c ~= "__IS_LISTEN__" and c ~= "__END_OF_TURN__" then
                    table.insert(results, c)
                end
            end
        end
    end
    return results
end

-- omni_init
io.stderr:write("[friday] omni_init...\n")
http_post("/v1/stream/omni_init", cjson.encode({
    media_type = 2, use_tts = true, duplex_mode = true,
    model_dir = "/data/models/MiniCPM-o-4_5-gguf",
    tts_bin_dir = "/data/models/MiniCPM-o-4_5-gguf/tts",
    tts_gpu_layers = 100, token2wav_device = "gpu:0",
    output_dir = "/tmp/omni_out2",
    voice_clone_prompt = "<|im_start|>system\n你是一个监控管理员，持续观察摄像头画面。正常情况下保持静默观察，不要主动说话。发现异常情况时简洁描述当前画面。\n<|audio_start|>",
    assistant_prompt = "<|audio_end|><|im_end|>\n",
}), 600)

-- warmup
io.stderr:write("[friday] warmup...\n")
os.execute("ffmpeg -f v4l2 -i /dev/video0 -vframes 1 -y /tmp/f.jpg 2>/dev/null")
http_post("/v1/stream/prefill",
    cjson.encode({audio_path_prefix="",img_path_prefix="/tmp/f.jpg",cnt=0}):gsub("\\/", "/"), 60)

-- force_listen_count=0
io.stderr:write("[friday] update_session_config...\n")
http_post("/v1/stream/update_session_config", '{"force_listen_count":0}', 30)

-- 主循环
io.stderr:write("[friday] 进入主循环\n")
local idx = 0
while true do
    idx = idx + 1

    -- 抓帧
    os.execute("ffmpeg -f v4l2 -i /dev/video0 -vframes 1 -y /tmp/f.jpg 2>/dev/null")
    -- 录音 (用测试音频)
    os.execute("cp /tmp/test_tone.wav /tmp/m.wav 2>/dev/null")

    -- prefill
    local pj = cjson.encode({
        audio_path_prefix = "/tmp/m.wav",
        img_path_prefix = "/tmp/f.jpg",
        cnt = idx,
    }):gsub("\\/", "/")
    http_post("/v1/stream/prefill", pj, 10)

    -- decode
    local ret = http_post("/v1/stream/decode", '{"debug_dir":"/tmp/omni_out2","stream":true}', 120)

    if ret > 0 then
        for _, c in ipairs(extract_text(ret)) do
            io.stderr:write("[AI] " .. c .. "\n")
            -- 写到文件供 GUI 读取
            local f = io.open("/tmp/friday_ai_text.txt", "w")
            if f then f:write(c); f:close() end
        end
    end

    -- TTS 播放
    os.execute("ls -t /tmp/omni_out2/round_*/tts_wav/wav_*.wav 2>/dev/null|head -1|xargs -r aplay -D plughw:0,3 -q 2>/dev/null &")

    if idx > 10 then
        os.execute(string.format("rm -f /tmp/f_%d.jpg /tmp/m_%d.wav", idx-10, idx-10))
    end

    os.execute("sleep 0.3")
end
